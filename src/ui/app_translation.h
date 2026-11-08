#pragma once

#include <QtCore/QLocale>
#include <QtCore/QStringList>

class QCoreApplication;

namespace MeetingUI::AppTranslation {

enum class Language { System, Chinese, English };

QString languageId(Language language);
Language languageFromId(const QString &id);
Language selectedLanguage();
// Save the next-launch preference; existing windows retain their language.
void setLanguage(Language language);
QLocale localeForLanguage(Language language, const QLocale &systemLocale = QLocale::system());
// --language overrides the saved preference for this launch only.
QLocale startupLocale(const QStringList &arguments, const QLocale &systemLocale = QLocale::system());
void install(QCoreApplication &application, const QLocale &locale,
	const QString &translationDirectory = QString());

} // namespace MeetingUI::AppTranslation
