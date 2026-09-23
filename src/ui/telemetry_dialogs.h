#pragma once

#include <QtCore/QVariant>

class QDialog;
class QWidget;

namespace MeetingUI {

QDialog *OpenTelemetryDetailsDialog(
	QWidget *parent,
	const QVariantMap &snapshot);
QDialog *OpenPostMeetingTelemetryDialog(QWidget *parent);

} // namespace MeetingUI
