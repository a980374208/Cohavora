#include "src/ui/app_translation.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QLibraryInfo>
#include <QtCore/QTranslator>
#include <QtCore/QVariant>

namespace MeetingUI::AppTranslation {
QLocale startupLocale(const QStringList &arguments) {
	for (int i = 1; i < arguments.size(); ++i) {
		const auto &argument = arguments.at(i);
		if (argument.startsWith(QStringLiteral("--language="))) {
			return QLocale(argument.mid(11));
		}
		if (argument == QStringLiteral("--language") && i + 1 < arguments.size()) {
			return QLocale(arguments.at(i + 1));
		}
	}
	return QLocale(QLocale::Chinese, QLocale::China);
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
