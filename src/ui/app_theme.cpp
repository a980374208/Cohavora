#include "src/ui/app_theme.h"

#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QObject>
#include <QtCore/QVariant>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtWidgets/QAbstractButton>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStyle>
#include <QtWidgets/QWidget>

#if defined(Q_OS_WIN)
#include <dwmapi.h>
#include <windows.h>
#endif

namespace MeetingUI::AppTheme {
namespace {

constexpr auto kToneProperty = "meetingUiTone";
constexpr auto kRoleProperty = "meetingUiRole";
constexpr auto kInstalledProperty = "meetingUiThemeInstalled";
constexpr auto kMenuStyledToneProperty = "meetingUiMenuStyledTone";
constexpr auto kDialogStyledToneProperty = "meetingUiDialogStyledTone";
constexpr auto kCenteredProperty = "meetingUiCentered";

const char *toneName(Tone tone) {
	return tone == Tone::Dark ? "dark" : "light";
}

Tone toneFromWidget(const QWidget *widget) {
	for (auto *current = widget; current; current = current->parentWidget()) {
		const auto value = current->property(kToneProperty).toByteArray();
		if (value == "dark") {
			return Tone::Dark;
		}
		if (value == "light") {
			return Tone::Light;
		}
	}
	return Tone::Light;
}

void refreshStyle(QWidget &widget) {
	if (auto *style = widget.style()) {
		style->unpolish(&widget);
		style->polish(&widget);
	}
	widget.update();
}

QScreen *screenForWindow(const QWidget &window) {
	const QWidget *anchor = window.parentWidget();
	while (anchor && !anchor->isWindow()) {
		anchor = anchor->parentWidget();
	}
	if (!anchor) {
		anchor = &window;
	}
	if (auto *screen = QGuiApplication::screenAt(anchor->frameGeometry().center())) {
		return screen;
	}
	if (auto *screen = QGuiApplication::screenAt(QCursor::pos())) {
		return screen;
	}
	return QGuiApplication::primaryScreen();
}

void setButtonRole(QAbstractButton *button, const char *role) {
	if (!button) {
		return;
	}
	button->setProperty(kRoleProperty, role);
	refreshStyle(*button);
}

const char *messageButtonRole(QMessageBox::ButtonRole role) {
	switch (role) {
	case QMessageBox::AcceptRole:
	case QMessageBox::YesRole:
	case QMessageBox::ApplyRole:
		return "primary";
	case QMessageBox::DestructiveRole:
		return "danger";
	case QMessageBox::HelpRole:
		return "link";
	default:
		return "secondary";
	}
}

const char *dialogButtonRole(QDialogButtonBox::ButtonRole role) {
	switch (role) {
	case QDialogButtonBox::AcceptRole:
	case QDialogButtonBox::YesRole:
	case QDialogButtonBox::ApplyRole:
		return "primary";
	case QDialogButtonBox::DestructiveRole:
		return "danger";
	case QDialogButtonBox::HelpRole:
		return "link";
	default:
		return "secondary";
	}
}

void styleDialogButtons(QDialogButtonBox &buttonBox) {
	for (auto *button : buttonBox.buttons()) {
		setButtonRole(button, dialogButtonRole(buttonBox.buttonRole(button)));
	}
}

void configureMessageBox(QMessageBox &box, Tone tone) {
	box.setProperty(kToneProperty, toneName(tone));
	box.setMinimumWidth(460);
	if (auto *label = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_label"))) {
		label->setWordWrap(true);
		label->setTextFormat(Qt::PlainText);
		label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
		label->setMinimumWidth(340);
		label->setMaximumWidth(520);
	}
	for (auto *button : box.buttons()) {
		setButtonRole(button, messageButtonRole(box.buttonRole(button)));
	}
	refreshStyle(box);
}

void configureInputDialog(QInputDialog &dialog, Tone tone) {
	dialog.setProperty(kToneProperty, toneName(tone));
	dialog.setMinimumWidth(420);
	for (auto *buttonBox : dialog.findChildren<QDialogButtonBox *>()) {
		styleDialogButtons(*buttonBox);
	}
	refreshStyle(dialog);
}

void configureProgressDialog(QProgressDialog &dialog, Tone tone) {
	dialog.setProperty(kToneProperty, toneName(tone));
	dialog.setMinimumWidth(380);
	for (auto *button : dialog.findChildren<QPushButton *>()) {
		setButtonRole(button, "secondary");
	}
	refreshStyle(dialog);
}

void applyNativeCorners(QWidget &widget, Tone tone) {
#if defined(Q_OS_WIN)
	if (widget.testAttribute(Qt::WA_TranslucentBackground)) {
		return;
	}
	auto handle = reinterpret_cast<HWND>(widget.winId());
	if (!handle) {
		return;
	}
	DWORD preference = 2; // DWMWCP_ROUND
	DwmSetWindowAttribute(
		handle,
		33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
		&preference,
		sizeof(preference));
	BOOL darkTitleBar = tone == Tone::Dark ? TRUE : FALSE;
	DwmSetWindowAttribute(
		handle,
		20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
		&darkTitleBar,
		sizeof(darkTitleBar));
#else
	Q_UNUSED(widget);
	Q_UNUSED(tone);
#endif
}

void centerOnParentOrScreen(QWidget &window) {
	auto *parentWindow = window.parentWidget();
	while (parentWindow && !parentWindow->isWindow()) {
		parentWindow = parentWindow->parentWidget();
	}
	if (!parentWindow || !parentWindow->isVisible()) {
		centerOnScreen(window);
		return;
	}

	auto size = window.frameGeometry().size();
	if (size.isEmpty()) {
		size = window.size();
	}
	const auto parentGeometry = parentWindow->frameGeometry();
	auto *screen = QGuiApplication::screenAt(parentGeometry.center());
	if (!screen) {
		screen = screenForWindow(window);
	}
	if (!screen) {
		return;
	}

	const auto available = screen->availableGeometry();
	const auto desiredX = parentGeometry.center().x() - size.width() / 2;
	const auto desiredY = parentGeometry.center().y() - size.height() / 2;
	const auto maxX = qMax(available.left(), available.right() - size.width() + 1);
	const auto maxY = qMax(available.top(), available.bottom() - size.height() + 1);
	window.move(
		qBound(available.left(), desiredX, maxX),
		qBound(available.top(), desiredY, maxY));
}

class ThemeEventFilter final : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override {
		if (auto *menu = qobject_cast<QMenu *>(watched)) {
			if (event->type() != QEvent::Polish && event->type() != QEvent::Show) {
				return QObject::eventFilter(watched, event);
			}
			const auto tone = toneFromWidget(menu);
			if (menu->property(kMenuStyledToneProperty).toByteArray() != toneName(tone)) {
				styleMenu(*menu, tone);
			}
			if (event->type() == QEvent::Show) {
				applyNativeCorners(*menu, tone);
			}
			return QObject::eventFilter(watched, event);
		}
		if (event->type() != QEvent::Polish && event->type() != QEvent::Show) {
			return QObject::eventFilter(watched, event);
		}

		auto *dialog = qobject_cast<QDialog *>(watched);
		if (!dialog) {
			return QObject::eventFilter(watched, event);
		}

		auto *box = qobject_cast<QMessageBox *>(dialog);
		auto *input = qobject_cast<QInputDialog *>(dialog);
		auto *progress = qobject_cast<QProgressDialog *>(dialog);
		// System prompts use the application's light form surface even when they
		// originate from a dark meeting panel.
		const auto tone = (box || input || progress)
			? Tone::Light
			: toneFromWidget(dialog);
		if (box || input || progress) {
			const auto styledTone = dialog->property(kDialogStyledToneProperty).toByteArray();
			if (styledTone != toneName(tone)) {
				dialog->setProperty(kDialogStyledToneProperty, toneName(tone));
				if (box) {
					configureMessageBox(*box, tone);
				} else if (input) {
					configureInputDialog(*input, tone);
				} else {
					configureProgressDialog(*progress, tone);
				}
			}
			if (box && event->type() == QEvent::Show) {
				box->adjustSize();
			}
		} else if (event->type() == QEvent::Show
			&& !dialog->property(kToneProperty).isValid()) {
			dialog->setProperty(kToneProperty, toneName(tone));
		}
		if (event->type() == QEvent::Show) {
			if (dialog->property("meetingUiAdaptive").toBool()) {
				auto *content = dialog->findChild<QWidget *>(QStringLiteral("adaptiveDialogContent"));
				auto desired = dialog->property("meetingUiPreferredSize").toSize();
				if (content) desired = desired.expandedTo(content->sizeHint());
				if (auto *screen = screenForWindow(*dialog)) {
					desired = desired.boundedTo(screen->availableGeometry().size() - QSize(32, 64));
				}
				dialog->resize(desired);
			}
			applyNativeCorners(*dialog, tone);
			if (!dialog->property(kCenteredProperty).toBool()) {
				centerOnParentOrScreen(*dialog);
				dialog->setProperty(kCenteredProperty, true);
			}
		}
		return QObject::eventFilter(watched, event);
	}
};

} // namespace

void makeDialogAdaptive(QDialog &dialog, QSize preferredSize) {
	if (!dialog.layout() || dialog.property("meetingUiAdaptive").toBool()) return;
	dialog.setProperty("meetingUiAdaptive", true);
	for (auto *label : dialog.findChildren<QLabel *>()) {
		if (label->maximumHeight() == QWIDGETSIZE_MAX) label->setWordWrap(true);
	}
	for (auto *form : dialog.findChildren<QFormLayout *>()) {
		form->setRowWrapPolicy(QFormLayout::WrapLongRows);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	}
	auto *content = new QWidget;
	content->setObjectName(QStringLiteral("adaptiveDialogContent"));
	content->setLayout(dialog.layout());
	content->layout()->setSizeConstraint(QLayout::SetMinAndMaxSize);
	auto *scroll = new QScrollArea(&dialog);
	scroll->setObjectName(QStringLiteral("adaptiveDialogScroll"));
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	scroll->viewport()->setAutoFillBackground(false);
	content->setAutoFillBackground(false);
	scroll->setWidget(content);
	auto *root = new QVBoxLayout(&dialog);
	root->setContentsMargins(0, 0, 0, 0);
	root->addWidget(scroll);
	dialog.setMinimumSize(280, 180);
	dialog.setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
	dialog.setSizeGripEnabled(!dialog.windowFlags().testFlag(Qt::FramelessWindowHint));
	dialog.setProperty("meetingUiPreferredSize", preferredSize);
	dialog.resize(preferredSize);
}

void install(QApplication &application) {
	if (application.property(kInstalledProperty).toBool()) {
		return;
	}
	application.setProperty(kInstalledProperty, true);
	QString sheet;
	for (const auto *path : { ":/meeting-ui/styles/widgets.qss", ":/meeting-ui/styles/application.qss" }) {
		QFile file(QString::fromLatin1(path));
		if (!file.open(QIODevice::ReadOnly)) {
			qFatal("Cannot load the application theme resource");
		}
		sheet += QString::fromUtf8(file.readAll()) + QLatin1Char('\n');
	}
	application.setStyleSheet(sheet);
	application.installEventFilter(new ThemeEventFilter(&application));
}

void setTone(QWidget &widget, Tone tone) {
	widget.setProperty(kToneProperty, toneName(tone));
	refreshStyle(widget);
}

void styleChoiceControls(QWidget &widget, Tone tone) {
	setTone(widget, tone);
}

void styleMenu(QMenu &menu, Tone tone) {
	menu.setProperty(kToneProperty, toneName(tone));
	menu.setProperty(kMenuStyledToneProperty, toneName(tone));
#if !defined(Q_OS_WIN)
	menu.setAttribute(Qt::WA_TranslucentBackground, true);
#endif
	refreshStyle(menu);
}

void centerOnScreen(QWidget &window) {
	auto *screen = screenForWindow(window);
	if (!screen) {
		return;
	}
	const auto available = screen->availableGeometry();
	auto size = window.frameGeometry().size();
	if (size.isEmpty()) {
		size = window.size();
	}
	const auto x = available.x() + qMax(0, (available.width() - size.width()) / 2);
	const auto y = available.y() + qMax(0, (available.height() - size.height()) / 2);
	window.move(x, y);
}

} // namespace MeetingUI::AppTheme
