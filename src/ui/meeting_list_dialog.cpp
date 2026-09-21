#include "src/ui/meeting_list_dialog.h"

#include "src/core/meeting_catalog_controller.h"
#include "src/net/session_manager.h"
#include "src/ui/meeting_list_delegate.h"
#include "src/ui/meeting_list_model.h"

#include <QtCore/QItemSelectionModel>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

namespace MeetingUI {
namespace {

QWidget *createListPage(
		MeetingListModel *model,
		QListView **view,
		QLabel **stateLabel,
		QWidget *parent) {
	auto *page = new QWidget(parent);
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(12, 12, 12, 12);
	layout->setSpacing(8);

	*stateLabel = new QLabel(page);
	(*stateLabel)->setWordWrap(true);
	(*stateLabel)->setTextFormat(Qt::PlainText);
	(*stateLabel)->setAlignment(Qt::AlignCenter);
	(*stateLabel)->setStyleSheet(QStringLiteral("color: #8f959e; padding: 12px;"));
	layout->addWidget(*stateLabel);

	*view = new QListView(page);
	(*view)->setModel(model);
	(*view)->setItemDelegate(new MeetingListDelegate(false, *view));
	(*view)->setSelectionMode(QAbstractItemView::SingleSelection);
	(*view)->setEditTriggers(QAbstractItemView::NoEditTriggers);
	(*view)->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	(*view)->setMouseTracking(true);
	(*view)->setUniformItemSizes(false);
	(*view)->setStyleSheet(R"(
		QListView { border: none; background: #ffffff; outline: none; }
		QListView::item { background: transparent; }
	)");
	layout->addWidget(*view, 1);
	return page;
}

QString stateText(
		const OpenMeeting::MeetingListViewState &state,
		const MeetingListModel &model,
		bool history) {
	if (state.refreshing && !state.hasSnapshot) return QString::fromUtf8("正在加载会议...");
	if (state.state == OpenMeeting::MeetingCatalogLoadState::Error && !state.hasSnapshot) {
		return QString::fromUtf8("会议加载失败，请检查网络后重试。");
	}
	if (model.rowCount() == 0) {
		if (model.isFiltering()) return QString::fromUtf8("没有匹配的会议。");
		return history
			? QString::fromUtf8("暂无由您创建的历史会议。")
			: QString::fromUtf8("暂无待进行的会议。");
	}
	if (state.refreshing) return QString::fromUtf8("正在刷新，当前显示最近一次结果。");
	if (state.error.code != 0) return QString::fromUtf8("刷新失败，当前显示最近一次结果。");
	return {};
}

} // namespace

MeetingListDialog::MeetingListDialog(
		OpenMeeting::MeetingCatalogController &controller,
		OpenMeeting::SessionManager &session,
		QWidget *parent)
	: QDialog(parent)
	, _controller(controller)
	, _session(session) {
	setWindowTitle(QString::fromUtf8("全部会议"));
	setModal(true);
	resize(720, 620);
	setMinimumSize(620, 500);
	setStyleSheet(R"(
		QDialog { background: #ffffff; }
		QLineEdit { min-height: 36px; border: 1px solid #dcdfe6; border-radius: 6px; padding: 0 10px; }
		QLineEdit:focus { border-color: #1677ff; }
		QTabWidget::pane { border: 1px solid #e5e8ec; border-radius: 6px; top: -1px; }
		QTabBar::tab { min-width: 150px; padding: 10px 14px; color: #606266; }
		QTabBar::tab:selected { color: #1677ff; font-weight: bold; }
		QPushButton { min-height: 34px; padding: 0 16px; border-radius: 6px; }
		QPushButton#primary { background: #1677ff; color: white; border: none; }
		QPushButton#secondary { background: #f2f3f5; color: #4e5969; border: none; }
	)");

	_upcomingModel = new MeetingListModel(this);
	_historyModel = new MeetingListModel(this);

	auto *root = new QVBoxLayout(this);
	root->setContentsMargins(24, 20, 24, 20);
	root->setSpacing(12);
	auto *heading = new QLabel(QString::fromUtf8("会议安排"), this);
	auto headingFont = heading->font();
	headingFont.setPixelSize(20);
	headingFont.setBold(true);
	heading->setFont(headingFont);
	root->addWidget(heading);

	auto *toolbar = new QHBoxLayout();
	_searchEdit = new QLineEdit(this);
	_searchEdit->setClearButtonEnabled(true);
	_searchEdit->setPlaceholderText(QString::fromUtf8("搜索会议主题或创建者"));
	_refreshButton = new QPushButton(QString::fromUtf8("刷新"), this);
	_refreshButton->setObjectName(QStringLiteral("secondary"));
	toolbar->addWidget(_searchEdit, 1);
	toolbar->addWidget(_refreshButton);
	root->addLayout(toolbar);

	_tabs = new QTabWidget(this);
	_tabs->addTab(
		createListPage(_upcomingModel, &_upcomingView, &_upcomingState, _tabs),
		QString::fromUtf8("未结束会议"));
	_tabs->addTab(
		createListPage(_historyModel, &_historyView, &_historyState, _tabs),
		QString::fromUtf8("我创建的历史会议"));
	root->addWidget(_tabs, 1);

	auto *buttons = new QHBoxLayout();
	buttons->addStretch();
	auto *closeButton = new QPushButton(QString::fromUtf8("关闭"), this);
	closeButton->setObjectName(QStringLiteral("secondary"));
	_viewButton = new QPushButton(QString::fromUtf8("查看详情"), this);
	_viewButton->setObjectName(QStringLiteral("secondary"));
	_viewButton->setEnabled(false);
	_joinButton = new QPushButton(QString::fromUtf8("加入会议"), this);
	_joinButton->setObjectName(QStringLiteral("primary"));
	_joinButton->setEnabled(false);
	buttons->addWidget(closeButton);
	buttons->addWidget(_viewButton);
	buttons->addWidget(_joinButton);
	root->addLayout(buttons);

	connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
	connect(_refreshButton, &QPushButton::clicked, this, [this] { refreshCurrent(); });
	connect(_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
		_upcomingModel->setSearchText(text);
		_historyModel->setSearchText(text);
		updateUpcoming();
		updateHistory();
	});
	connect(_tabs, &QTabWidget::currentChanged, this, [this](int) {
		const auto &state = _tabs->currentIndex() == 0
			? _controller.upcomingState() : _controller.historyState();
		if (state.state == OpenMeeting::MeetingCatalogLoadState::Idle) refreshCurrent();
		updateCommandButtons();
	});
	connect(_upcomingView, &QListView::activated, this,
		[this](const QModelIndex &index) { activateIndex(_upcomingView, index); });
	connect(_historyView, &QListView::activated, this,
		[this](const QModelIndex &index) { activateIndex(_historyView, index); });
	auto selectionChanged = [this] { updateCommandButtons(); };
	connect(_upcomingView->selectionModel(), &QItemSelectionModel::selectionChanged,
		this, [selectionChanged] { selectionChanged(); });
	connect(_historyView->selectionModel(), &QItemSelectionModel::selectionChanged,
		this, [selectionChanged] { selectionChanged(); });
	connect(_viewButton, &QPushButton::clicked, this, [this] {
		auto *view = _tabs->currentIndex() == 0 ? _upcomingView : _historyView;
		activateIndex(view, view->currentIndex());
	});
	connect(_joinButton, &QPushButton::clicked, this,
		[this] { requestSelectedJoin(); });

	connect(&_controller, &OpenMeeting::MeetingCatalogController::upcomingChanged,
		this, [this] { updateUpcoming(); });
	connect(&_controller, &OpenMeeting::MeetingCatalogController::historyChanged,
		this, [this] { updateHistory(); });
	connect(&_session, &OpenMeeting::SessionManager::authenticationReset,
		this, [this](quint64) { reject(); });

	updateUpcoming();
	updateHistory();
	if (_controller.upcomingState().state == OpenMeeting::MeetingCatalogLoadState::Idle) {
		_controller.refreshUpcoming();
	}
}

void MeetingListDialog::updateUpcoming() {
	updatePage(
		_controller.upcomingState(),
		_upcomingModel,
		_upcomingView,
		_upcomingState,
		false);
}

void MeetingListDialog::updateHistory() {
	updatePage(
		_controller.historyState(),
		_historyModel,
		_historyView,
		_historyState,
		true);
}

void MeetingListDialog::updatePage(
		const OpenMeeting::MeetingListViewState &state,
		MeetingListModel *model,
		QListView *view,
		QLabel *stateLabel,
		bool history) {
	model->setMeetings(state.meetings, history, history ? _session.userId() : QString());
	const auto message = stateText(state, *model, history);
	stateLabel->setText(message);
	stateLabel->setVisible(!message.isEmpty());
	view->setVisible(model->rowCount() > 0);
	if ((_tabs->currentIndex() == (history ? 1 : 0))) {
		_refreshButton->setEnabled(!state.refreshing);
		updateCommandButtons();
	}
}

void MeetingListDialog::activateIndex(QListView *view, const QModelIndex &index) {
	if (!index.isValid() || index.model() != view->model()) return;
	const auto meetingId = index.data(MeetingIdRole).toString();
	if (!meetingId.isEmpty()) emit meetingActivated(meetingId);
}

void MeetingListDialog::requestSelectedJoin() {
	if (_tabs->currentIndex() != 0) return;
	const auto index = _upcomingView->currentIndex();
	const auto *meeting = _upcomingModel->meetingAt(index.row());
	if (!meeting || (meeting->status != OpenMeeting::MeetingStatus::Scheduled &&
		meeting->status != OpenMeeting::MeetingStatus::InProgress)) return;
	emit joinRequested(meeting->meetingId);
}

void MeetingListDialog::updateCommandButtons() {
	auto *view = _tabs->currentIndex() == 0 ? _upcomingView : _historyView;
	const auto index = view->currentIndex();
	_viewButton->setEnabled(index.isValid());

	bool canJoin = false;
	if (_tabs->currentIndex() == 0 && index.isValid()) {
		if (const auto *meeting = _upcomingModel->meetingAt(index.row())) {
			canJoin = meeting->status == OpenMeeting::MeetingStatus::Scheduled ||
				meeting->status == OpenMeeting::MeetingStatus::InProgress;
		}
	}
	_joinButton->setEnabled(canJoin);
}

void MeetingListDialog::refreshCurrent() {
	if (_tabs->currentIndex() == 0) {
		if (!_controller.upcomingState().refreshing) _controller.refreshUpcoming();
	} else {
		if (!_controller.historyState().refreshing) _controller.refreshHistory();
	}
}

} // namespace MeetingUI
