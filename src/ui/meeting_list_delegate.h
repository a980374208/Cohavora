#pragma once

#include <QtWidgets/QStyledItemDelegate>

namespace MeetingUI {

class MeetingListDelegate final : public QStyledItemDelegate {
public:
	explicit MeetingListDelegate(bool compact, QObject *parent = nullptr);

	QSize sizeHint(
		const QStyleOptionViewItem &option,
		const QModelIndex &index) const override;
	void paint(
		QPainter *painter,
		const QStyleOptionViewItem &option,
		const QModelIndex &index) const override;

private:
	bool _compact = false;
};

} // namespace MeetingUI
