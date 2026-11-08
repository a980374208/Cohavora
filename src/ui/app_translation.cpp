#include "src/ui/app_translation.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QLibraryInfo>
#include <QtCore/QSettings>
#include <QtCore/QTranslator>
#include <QtCore/QVariant>

namespace MeetingUI::AppTranslation {
namespace {
constexpr auto kLanguageSetting = "ui/language";
}

QString languageId(Language language) {
	switch (language) {
	case Language::Chinese: return QStringLiteral("zh_CN");
	case Language::English: return QStringLiteral("en_US");
	case Language::System: return QStringLiteral("system");
	}
	return QStringLiteral("system");
}

Language languageFromId(const QString &id) {
	if (id == QStringLiteral("zh_CN")) return Language::Chinese;
	if (id == QStringLiteral("en_US")) return Language::English;
	return Language::System;
}

Language selectedLanguage() {
	return languageFromId(QSettings().value(QString::fromLatin1(kLanguageSetting)).toString());
}

void setLanguage(Language language) {
	QSettings().setValue(QString::fromLatin1(kLanguageSetting), languageId(language));
}

QLocale localeForLanguage(Language language, const QLocale &systemLocale) {
	if (language == Language::System) {
		// Prefer the OS display language over its regional formatting locale.
		const auto uiLanguages = systemLocale.uiLanguages();
		const auto uiLocale = uiLanguages.isEmpty() ? systemLocale : QLocale(uiLanguages.front());
		language = uiLocale.language() == QLocale::Chinese ? Language::Chinese : Language::English;
	}
	// All Chinese variants use the available Simplified Chinese catalog.
	return language == Language::Chinese
		? QLocale(QLocale::Chinese, QLocale::China)
		: QLocale(QLocale::English, QLocale::UnitedStates);
}

QLocale startupLocale(const QStringList &arguments, const QLocale &systemLocale) {
	const auto overrideLocale = [&](const QString &id) {
		if (id == QStringLiteral("system")) return localeForLanguage(Language::System, systemLocale);
		return localeForLanguage(QLocale(id).language() == QLocale::Chinese
			? Language::Chinese : Language::English, systemLocale);
	};
	for (int i = 1; i < arguments.size(); ++i) {
		const auto &argument = arguments.at(i);
		if (argument.startsWith(QStringLiteral("--language="))) {
			return overrideLocale(argument.mid(11));
		}
		if (argument == QStringLiteral("--language") && i + 1 < arguments.size()) {
			return overrideLocale(arguments.at(i + 1));
		}
	}
	return localeForLanguage(selectedLanguage(), systemLocale);
}

void install(QCoreApplication &application, const QLocale &locale,
	const QString &translationDirectory) {
	if (application.property("meetingUiTranslationInstalled").toBool()) return;
	QLocale::setDefault(locale);
	const auto directory = translationDirectory.isEmpty()
		? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("translations"))
		: translationDirectory;
	const auto load = [&](const QStringList &names, const QStringList &directories) {
		auto *translator = new QTranslator(&application);
		for (const auto &path : directories) {
			for (const auto &name : names) {
				if (translator->load(locale, name, QStringLiteral("_"), path)) {
					application.installTranslator(translator);
					return true;
				}
			}
		}
		delete translator; // Missing catalogs fall back to source text.
		return false;
	};
	load({ QStringLiteral("qt"), QStringLiteral("qtbase") },
		{ directory, QStringLiteral(":/meeting-ui/translations"),
			QLibraryInfo::location(QLibraryInfo::TranslationsPath) });
	if (!load({ QStringLiteral("cohavora") },
			{ directory, QStringLiteral(":/meeting-ui/translations") })) {
		// Legacy catalogs are accepted only as external deployment artifacts.
		load({ QStringLiteral("livekit_meeting") }, { directory });
	}
	application.setProperty("meetingUiTranslationInstalled", true);
}

} // namespace MeetingUI::AppTranslation
