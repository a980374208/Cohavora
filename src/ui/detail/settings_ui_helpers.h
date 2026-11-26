#pragma once

#include "src/ui/app_icons.h"
#include "src/ui/app_theme.h"

#include <openssl/crypto.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QPoint>
#include <QtCore/QSignalBlocker>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QToolTip>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <functional>

namespace MeetingUI {

inline void wipeQString(QString &text) {
	if (!text.isEmpty()) {
		OPENSSL_cleanse(text.data(), static_cast<size_t>(text.size()) * sizeof(QChar));
	}
	text.clear();
}

inline void wipeAndClearLineEdit(QLineEdit *edit) {
	if (!edit) {
		return;
	}
	const QSignalBlocker blocker(edit);
	QString text = edit->text();
	// setText resets the undo history. Qt/IME historical copies cannot be guaranteed erased.
	edit->setText(QString(text.size(), QChar(0)));
	edit->clear();
	wipeQString(text);
}

inline QScrollArea *makeScrollablePage(QWidget *parent) {
	auto *scroll = new QScrollArea(parent);
	scroll->setObjectName(QStringLiteral("settingsScrollArea"));
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	auto *content = new QWidget(scroll->viewport());
	content->setObjectName(QStringLiteral("settingsScrollContent"));
	auto *layout = new QVBoxLayout(content);
	layout->setContentsMargins(20, 18, 20, 20);
	layout->setSpacing(14);
	scroll->setWidget(content);
	return scroll;
}

inline QFrame *makeSettingsCard(
		const QString &title,
		QWidget *parent,
		QVBoxLayout **outBody,
		QWidget *headerRight = nullptr,
		QWidget *titleAccessory = nullptr) {
	auto *card = new QFrame(parent);
	card->setObjectName(QStringLiteral("settingsCard"));
	auto *cardLayout = new QVBoxLayout(card);
	cardLayout->setContentsMargins(0, 0, 0, 0);
	cardLayout->setSpacing(0);

	if (!title.isEmpty() || headerRight || titleAccessory) {
		auto *header = new QWidget(card);
		header->setObjectName(QStringLiteral("settingsCardHeader"));
		auto *headerLayout = new QHBoxLayout(header);
		headerLayout->setContentsMargins(16, 10, 14, 10);
		headerLayout->setSpacing(6);
		if (!title.isEmpty()) {
			auto *titleLabel = new QLabel(title, header);
			titleLabel->setObjectName(QStringLiteral("settingsCardTitle"));
			titleLabel->setWordWrap(false);
			titleLabel->setProperty("keepSingleLine", true);
			headerLayout->addWidget(titleLabel);
		}
		if (titleAccessory) {
			titleAccessory->setParent(header);
			headerLayout->addWidget(titleAccessory, 0, Qt::AlignVCenter);
		}
		headerLayout->addStretch(1);
		if (headerRight) {
			headerLayout->addWidget(headerRight);
		}
		cardLayout->addWidget(header);
	}

	auto *body = new QWidget(card);
	body->setObjectName(QStringLiteral("settingsCardBody"));
	auto *bodyLayout = new QVBoxLayout(body);
	bodyLayout->setContentsMargins(16, 14, 16, 14);
	bodyLayout->setSpacing(10);
	cardLayout->addWidget(body);

	if (outBody) {
		*outBody = bodyLayout;
	}
	return card;
}

inline void repolishSettingsWidget(QWidget *w) {
	if (!w) return;
	w->style()->unpolish(w);
	w->style()->polish(w);
	w->update();
}

inline QString formatHintToolTip(const QString &plainText) {
	const QString trimmed = plainText.trimmed();
	if (trimmed.isEmpty()) {
		return {};
	}
	QString escaped = trimmed.toHtmlEscaped();
	escaped.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	escaped.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
	return QStringLiteral("<qt>%1</qt>").arg(escaped);
}

class HintIconButton final : public QToolButton {
public:
	explicit HintIconButton(int iconSizePx, QWidget *parent = nullptr)
		: QToolButton(parent) {
		setProperty("copilotHintIcon", true);
		setCursor(Qt::PointingHandCursor);
		setFocusPolicy(Qt::NoFocus);
		setFixedSize(20, 20);
		setIconSize(QSize(iconSizePx, iconSizePx));
		refreshIcon();
		AppTheme::observeChanges(*this, [this] { refreshIcon(); });
	}

	void setHint(const QString &plainText, const QString &tone = QString()) {
		const QString trimmed = plainText.trimmed();
		const QString nextRich = formatHintToolTip(trimmed);
		const bool wasShowingSelf =
			QToolTip::isVisible() && !_richTip.isEmpty() && QToolTip::text() == _richTip;
		_plainText = trimmed;
		_richTip = nextRich;
		setToolTip(_richTip);
		setAccessibleDescription(_plainText);
		if (_tone != tone) {
			_tone = tone;
			setProperty("hintTone", _tone);
			if (auto *st = style()) {
				st->unpolish(this);
				st->polish(this);
			}
		}
		refreshIcon();
		if (_hovered && isVisible()) {
			if (!_richTip.isEmpty()) {
				showHintNow();
			} else if (wasShowingSelf) {
				QToolTip::hideText();
			}
		} else if (wasShowingSelf) {
			QToolTip::hideText();
		}
	}

protected:
	bool event(QEvent *event) override {
		switch (event->type()) {
		case QEvent::Enter:
			_hovered = true;
			refreshIcon();
			showHintNow();
			break;
		case QEvent::Leave:
		case QEvent::Hide:
			_hovered = false;
			refreshIcon();
			if (QToolTip::isVisible() && !_richTip.isEmpty() && QToolTip::text() == _richTip) {
				QToolTip::hideText();
			}
			break;
		case QEvent::ToolTip:
			if (!_richTip.isEmpty()) {
				if (!QToolTip::isVisible() || QToolTip::text() != _richTip) {
					showHintNow();
				}
				event->accept();
				return true;
			}
			break;
		case QEvent::MouseButtonPress:
			if (!_richTip.isEmpty()) {
				showHintNow();
			}
			break;
		default:
			break;
		}
		return QToolButton::event(event);
	}

private:
	void refreshIcon() {
		auto iconKind = AppTheme::Icon::Information;
		auto role = _hovered ? AppTheme::ColorRole::Accent : AppTheme::ColorRole::TextMuted;
		if (_tone == QStringLiteral("warning")) {
			role = AppTheme::ColorRole::Warning;
		} else if (_tone == QStringLiteral("error")) {
			iconKind = AppTheme::Icon::ErrorCircle;
			role = AppTheme::ColorRole::Danger;
		} else if (_tone == QStringLiteral("pending")) {
			role = AppTheme::ColorRole::Accent;
		}
		setIcon(AppTheme::icon(iconKind, role));
	}

	void showHintNow() {
		if (_richTip.isEmpty() || !isVisible() || QApplication::activePopupWidget() != nullptr) {
			return;
		}
		QToolTip::showText(mapToGlobal(QPoint(width() / 2, height())), _richTip, this, rect());
	}

	QString _plainText;
	QString _richTip;
	QString _tone;
	bool _hovered = false;
};

inline HintIconButton *makeHintIcon(
		const QString &accessibleTitle,
		const QString &hintText,
		QWidget *parent,
		int iconSizePx = 14) {
	auto *icon = new HintIconButton(iconSizePx, parent);
	icon->setAccessibleName(accessibleTitle);
	if (!hintText.isEmpty()) {
		icon->setHint(hintText);
	}
	return icon;
}

inline void setHintIcon(QToolButton *button, const QString &plainText, const QString &tone = QString()) {
	if (!button) {
		return;
	}
	static_cast<HintIconButton *>(button)->setHint(plainText, tone);
}

inline QHBoxLayout *makeTitleWithHintRow(
		QWidget *titleWidget,
		QToolButton *hintIcon,
		bool addTrailingStretch = true) {
	if (auto *label = qobject_cast<QLabel *>(titleWidget)) {
		label->setWordWrap(false);
		label->setProperty("keepSingleLine", true);
	}
	auto *row = new QHBoxLayout();
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(4);
	row->addWidget(titleWidget, 0, Qt::AlignVCenter);
	row->addWidget(hintIcon, 0, Qt::AlignVCenter);
	if (addTrailingStretch) {
		row->addStretch(1);
	}
	return row;
}

class SettingsCollapsibleSection final : public QWidget {
public:
	explicit SettingsCollapsibleSection(
		const QString &title,
		const QString &toggleObjectName,
		QWidget *parent = nullptr)
		: QWidget(parent) {
		setObjectName(QStringLiteral("settingsCollapsibleSection"));
		auto *rootLayout = new QVBoxLayout(this);
		rootLayout->setContentsMargins(0, 0, 0, 0);
		rootLayout->setSpacing(6);

		_headerBar = new QFrame(this);
		_headerBar->setObjectName(QStringLiteral("settingsCollapsibleBar"));
		_headerBar->setCursor(Qt::PointingHandCursor);
		_headerBar->installEventFilter(this);
		auto *barLayout = new QHBoxLayout(_headerBar);
		barLayout->setContentsMargins(8, 3, 8, 3);
		barLayout->setSpacing(6);

		_toggleBtn = new QPushButton(title, _headerBar);
		_toggleBtn->setObjectName(toggleObjectName);
		_toggleBtn->setCheckable(true);
		_toggleBtn->setChecked(false);
		_toggleBtn->setAutoDefault(false);
		_toggleBtn->setCursor(Qt::PointingHandCursor);
		_toggleBtn->setProperty("collapsibleToggle", true);
		_toggleBtn->setIconSize(QSize(13, 13));
		barLayout->addWidget(_toggleBtn, 0, Qt::AlignVCenter);

		barLayout->addStretch(1);

		_summaryLabel = new QLabel(_headerBar);
		_summaryLabel->setObjectName(QStringLiteral("settingsCollapsibleSummary"));
		_summaryLabel->setWordWrap(false);
		_summaryLabel->setProperty("keepSingleLine", true);
		_summaryLabel->setProperty("customState", QStringLiteral("default"));
		_summaryLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		barLayout->addWidget(_summaryLabel, 0, Qt::AlignVCenter);

		_resetBtn = new QPushButton(QCoreApplication::translate("MeetingUI", "Reset"), _headerBar);
		_resetBtn->setObjectName(QStringLiteral("settingsCollapsibleResetBtn"));
		_resetBtn->setAutoDefault(false);
		_resetBtn->setCursor(Qt::PointingHandCursor);
		_resetBtn->setToolTip(QCoreApplication::translate("MeetingUI", "Restore default values"));
		_resetBtn->hide();
		barLayout->addWidget(_resetBtn, 0, Qt::AlignVCenter);

		rootLayout->addWidget(_headerBar);

		_bodyFrame = new QFrame(this);
		_bodyFrame->setObjectName(QStringLiteral("settingsCollapsibleBody"));
		_bodyLayout = new QVBoxLayout(_bodyFrame);
		_bodyLayout->setContentsMargins(12, 8, 2, 4);
		_bodyLayout->setSpacing(8);
		_bodyFrame->hide();
		rootLayout->addWidget(_bodyFrame);

		connect(_toggleBtn, &QPushButton::toggled, this, [this](bool expanded) {
			_bodyFrame->setVisible(expanded);
			refreshIcons();
			if (_resetBtn) {
				_resetBtn->setVisible(expanded && _isCustomized);
			}
		});
		connect(_resetBtn, &QPushButton::clicked, this, [this] {
			if (_resetHandler) {
				_resetHandler();
			}
		});

		refreshIcons();
		AppTheme::observeChanges(*this, [this] { refreshIcons(); });
	}

	QVBoxLayout *bodyLayout() const { return _bodyLayout; }
	QWidget *bodyWidget() const { return _bodyFrame; }

	bool isExpanded() const { return _toggleBtn && _toggleBtn->isChecked(); }

	void setExpanded(bool expanded) {
		if (_toggleBtn && _toggleBtn->isChecked() != expanded) {
			_toggleBtn->setChecked(expanded);
		}
	}

	void setSummary(const QString &defaultText, const QStringList &customizedItems) {
		_isCustomized = !customizedItems.isEmpty();
		if (_summaryLabel) {
			if (!_isCustomized) {
				_summaryLabel->setText(defaultText);
				_summaryLabel->setToolTip(QString());
				_summaryLabel->setProperty("customState", QStringLiteral("default"));
			} else {
				_summaryLabel->setText(
					QCoreApplication::translate("MeetingUI", "Customized (%1)").arg(customizedItems.size()));
				_summaryLabel->setToolTip(customizedItems.join(QStringLiteral(" · ")));
				_summaryLabel->setProperty("customState", QStringLiteral("custom"));
			}
			if (auto *st = _summaryLabel->style()) {
				st->unpolish(_summaryLabel);
				st->polish(_summaryLabel);
			}
		}
		if (_resetBtn) {
			_resetBtn->setVisible(isExpanded() && _isCustomized);
		}
	}

	void setResetHandler(std::function<void()> handler) {
		_resetHandler = std::move(handler);
	}

	void refreshIcons() {
		if (_toggleBtn) {
			const auto iconKind = _toggleBtn->isChecked()
				? AppTheme::Icon::ChevronDown
				: AppTheme::Icon::ChevronRight;
			_toggleBtn->setIcon(AppTheme::icon(iconKind, AppTheme::ColorRole::TextSecondary));
		}
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (watched == _headerBar && event->type() == QEvent::MouseButtonRelease) {
			const auto *me = static_cast<QMouseEvent *>(event);
			if (me->button() == Qt::LeftButton && _headerBar->rect().contains(me->pos())) {
				QWidget *hit = _headerBar->childAt(me->pos());
				if (!hit || (hit != _toggleBtn && hit != _resetBtn)) {
					if (_toggleBtn) {
						_toggleBtn->toggle();
					}
					return true;
				}
			}
		}
		return QWidget::eventFilter(watched, event);
	}

private:
	QFrame *_headerBar = nullptr;
	QPushButton *_toggleBtn = nullptr;
	QLabel *_summaryLabel = nullptr;
	QPushButton *_resetBtn = nullptr;
	QFrame *_bodyFrame = nullptr;
	QVBoxLayout *_bodyLayout = nullptr;
	bool _isCustomized = false;
	std::function<void()> _resetHandler;
};

} // namespace MeetingUI
