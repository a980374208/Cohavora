#pragma once

#include "src/ui/app_theme.h"

#include <QtGui/QColor>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>

namespace MeetingUI::detail {

inline QColor themeColor(AppTheme::ColorRole role, int alpha = -1) {
	auto result = AppTheme::color(role);
	if (alpha >= 0) {
		result.setAlpha(alpha);
	}
	return result;
}

inline void ShowMeetingWarning(
		QWidget *parent,
		const char *id,
		const char *dismissId,
		const QString &title,
		const QString &text) {
	auto *message = new QMessageBox(
		QMessageBox::Warning,
		title,
		text,
		QMessageBox::Ok,
		parent);
	message->setObjectName(QString::fromLatin1(id));
	message->setAttribute(Qt::WA_DeleteOnClose);
	AppTheme::setTone(*message, AppTheme::Tone::Dark);
	message->button(QMessageBox::Ok)->setObjectName(QString::fromLatin1(dismissId));
	message->open();
}

} // namespace MeetingUI::detail
