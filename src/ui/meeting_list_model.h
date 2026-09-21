#pragma once

#include "src/net/meeting_types.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QDateTime>

#include <vector>

namespace MeetingUI {

enum MeetingItemRole {
	MeetingIdRole = Qt::UserRole + 1,
	MeetingTitleRole,
	MeetingCreatorRole,
	MeetingScheduledTimeRole,
	MeetingTimeDisplayRole,
	MeetingDurationRole,
	MeetingStatusRole,
	MeetingRepeatRole,
	MeetingDateGroupRole,
	MeetingShowDateHeaderRole,
};

class MeetingListModel final : public QAbstractListModel {
public:
	explicit MeetingListModel(QObject *parent = nullptr);

	int rowCount(const QModelIndex &parent = QModelIndex()) const override;
	QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
	QHash<int, QByteArray> roleNames() const override;

	void setMeetings(
		const OpenMeeting::MeetingList &meetings,
		bool history,
		const QString &historyCreatorUserId = QString());
	void setSearchText(const QString &text);

	const OpenMeeting::MeetingRecord *meetingAt(int row) const;
	int sourceCount() const { return static_cast<int>(_source.size()); }
	bool isFiltering() const { return !_searchText.isEmpty(); }

	static QDateTime scheduledDateTime(const OpenMeeting::MeetingRecord &meeting);
	static QString statusText(OpenMeeting::MeetingStatus status);
	static QString repeatText(
		const OpenMeeting::MeetingRepeatRule &rule,
		const QString &timeZone = QString());

private:
	struct Row {
		OpenMeeting::MeetingRecord meeting;
		QString dateGroup;
		bool showDateHeader = false;
	};

	void rebuild();

	OpenMeeting::MeetingList _source;
	std::vector<Row> _rows;
	QString _searchText;
	QString _historyCreatorUserId;
	bool _history = false;
};

} // namespace MeetingUI
