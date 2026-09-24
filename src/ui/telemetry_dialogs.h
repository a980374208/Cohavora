#pragma once

#include <QtCore/QVariant>

class QDialog;
class QString;
class QWidget;

namespace MeetingUI {

QString LocalizeTelemetryDisplayText(const QString &value);
QString LocalizeTelemetryFieldName(const QString &field);
QDialog *OpenTelemetryDetailsDialog(
	QWidget *parent,
	const QVariantMap &snapshot);
QDialog *OpenPostMeetingTelemetryDialog(QWidget *parent);

} // namespace MeetingUI
