#include "src/ui/app_theme.h"
#include <QtCore/QCoreApplication>
#include "src/ui/meeting_list_delegate.h"

#include "src/ui/meeting_list_model.h"

#include <QtGui/QPainter>
#include <QtWidgets/QApplication>

namespace MeetingUI {
namespace Theme = MeetingUI::AppTheme;

MeetingListDelegate::MeetingListDelegate(bool compact, QObject *parent)
	: QStyledItemDelegate(parent)
	, _compact(compact) {
}

QSize MeetingListDelegate::sizeHint(
		const QStyleOptionViewItem &option,
		const QModelIndex &index) const {
	const auto headerHeight = index.data(MeetingShowDateHeaderRole).toBool() ? 27 : 5;
	return QSize(option.rect.width(), headerHeight + (_compact ? 62 : 72));
}

void MeetingListDelegate::paint(
		QPainter *painter,
		const QStyleOptionViewItem &option,
		const QModelIndex &index) const {
	painter->save();
	painter->setRenderHint(QPainter::Antialiasing);
	painter->setRenderHint(QPainter::TextAntialiasing);

	auto content = option.rect.adjusted(4, 0, -4, -4);
	if (index.data(MeetingShowDateHeaderRole).toBool()) {
		QFont headerFont = option.font;
		headerFont.setPixelSize(12);
		headerFont.setBold(true);
		painter->setFont(headerFont);
		painter->setPen(Theme::color(Theme::ColorRole::TextSecondary));
		painter->drawText(
			QRect(content.left() + 4, content.top(), content.width() - 8, 22),
			Qt::AlignLeft | Qt::AlignVCenter,
			index.data(MeetingDateGroupRole).toString());
		content.setTop(content.top() + 27);
	} else {
		content.setTop(content.top() + 5);
	}

	const bool selected = option.state.testFlag(QStyle::State_Selected);
	const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
	painter->setPen(QPen(selected ? Theme::color(Theme::ColorRole::Accent) : Theme::color(Theme::ColorRole::Border), 1));
	painter->setBrush(selected
		? Theme::color(Theme::ColorRole::AccentSoft)
		: (hovered ? Theme::color(Theme::ColorRole::Hover) : Theme::color(Theme::ColorRole::Surface)));
	painter->drawRoundedRect(content, 7, 7);

	const auto title = index.data(MeetingTitleRole).toString();
	const auto creator = index.data(MeetingCreatorRole).toString();
	const auto status = index.data(MeetingStatusRole).toString();

	const int left = content.left() + 13;
	const int right = content.right() - 13;
	QFont timeFont = option.font;
	timeFont.setPixelSize(14);
	timeFont.setBold(true);
	painter->setFont(timeFont);
	painter->setPen(Theme::color(Theme::ColorRole::Accent));
	painter->drawText(
		QRect(left, content.top() + 8, 52, 23),
		Qt::AlignLeft | Qt::AlignVCenter,
		index.data(MeetingTimeDisplayRole).toString());

	QFont titleFont = option.font;
	titleFont.setPixelSize(14);
	titleFont.setBold(true);
	painter->setFont(titleFont);
	painter->setPen(Theme::color(Theme::ColorRole::Text));
	painter->drawText(
		QRect(left + 60, content.top() + 8, right - left - 120, 23),
		Qt::AlignLeft | Qt::AlignVCenter,
		painter->fontMetrics().elidedText(title, Qt::ElideRight, right - left - 120));

	QFont stateFont = option.font;
	stateFont.setPixelSize(11);
	stateFont.setBold(false);
	painter->setFont(stateFont);
	painter->setPen(status == QCoreApplication::translate("MeetingUI", "In Progress")
		? Theme::color(Theme::ColorRole::Success)
		: Theme::color(Theme::ColorRole::TextSecondary));
	painter->drawText(
		QRect(right - 70, content.top() + 8, 70, 23),
		Qt::AlignRight | Qt::AlignVCenter,
		status);

	auto subtitle = creator.trimmed().isEmpty()
		? QCoreApplication::translate("MeetingUI", "Meeting ID %1").arg(index.data(MeetingIdRole).toString())
		: QCoreApplication::translate("MeetingUI", "%1 · Meeting ID %2").arg(creator, index.data(MeetingIdRole).toString());
	const auto repeat = index.data(MeetingRepeatRole).toString();
	if (repeat != QCoreApplication::translate("MeetingUI", "Does Not Repeat")) {
		subtitle += QString::fromUtf8(" · %1").arg(repeat);
	}
	painter->setPen(Theme::color(Theme::ColorRole::TextMuted));
	painter->drawText(
		QRect(left + 60, content.top() + 34, right - left - 60, 20),
		Qt::AlignLeft | Qt::AlignVCenter,
		painter->fontMetrics().elidedText(subtitle, Qt::ElideRight, right - left - 60));

	painter->restore();
}

} // namespace MeetingUI
