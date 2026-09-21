#pragma once

#include <QtCore/QCoreApplication>

#include "base/basic_types.h"
#include "ui/integration.h"
#include <QtCore/QObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QDir>
#include <QtCore/QTimer>
#include <QtWidgets/QWidget>

namespace MeetingUI {

class MeetingUiIntegration final : public Ui::Integration {
public:
	MeetingUiIntegration() = default;
	~MeetingUiIntegration() = default;

	void postponeCall(FnMut<void()> &&callable) override {
		QTimer::singleShot(0, [c = std::move(callable)]() mutable {
			c();
		});
	}

	void registerLeaveSubscription(not_null<QWidget*> widget) override {
	}
	void unregisterLeaveSubscription(not_null<QWidget*> widget) override {
	}

	QString emojiCacheFolder() override {
		const auto path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/emoji";
		QDir().mkpath(path);
		return path;
	}
	QString openglCheckFilePath() override {
		return QString();
	}
	QString angleBackendFilePath() override {
		return QString();
	}

	void textActionsUpdated() override {
	}
	void activationFromTopPanel() override {
	}

	void touchCounterIncrement() override {
		++_touchCounter;
	}
	int touchCounterNow() override {
		return _touchCounter;
	}

	bool screenIsLocked() override {
		return false;
	}

	QString phraseContextCopyText() override { return QCoreApplication::translate("MeetingUI", "Copy"); }
	QString phraseContextCopyEmail() override { return QCoreApplication::translate("MeetingUI", "Copy Email"); }
	QString phraseContextCopyLink() override { return QCoreApplication::translate("MeetingUI", "Copy Link"); }
	QString phraseContextCopySelected() override { return QCoreApplication::translate("MeetingUI", "Copy Selection"); }
	QString phraseFormattingTitle() override { return QCoreApplication::translate("MeetingUI", "Formatting"); }
	QString phraseFormattingLinkCreate() override { return QCoreApplication::translate("MeetingUI", "Create Link"); }
	QString phraseFormattingLinkEdit() override { return QCoreApplication::translate("MeetingUI", "Edit Link"); }
	QString phraseFormattingClear() override { return QCoreApplication::translate("MeetingUI", "Clear Formatting"); }
	QString phraseFormattingBold() override { return QCoreApplication::translate("MeetingUI", "Bold"); }
	QString phraseFormattingItalic() override { return QCoreApplication::translate("MeetingUI", "Italic"); }
	QString phraseFormattingUnderline() override { return QCoreApplication::translate("MeetingUI", "Underline"); }
	QString phraseFormattingStrikeOut() override { return QCoreApplication::translate("MeetingUI", "Strikethrough"); }
	QString phraseFormattingBlockquote() override { return QCoreApplication::translate("MeetingUI", "Quote"); }
	QString phraseFormattingMonospace() override { return QCoreApplication::translate("MeetingUI", "Monospace"); }
	QString phraseFormattingSpoiler() override { return QCoreApplication::translate("MeetingUI", "Spoiler"); }
	QString phraseFormattingDate() override { return QCoreApplication::translate("MeetingUI", "Date"); }
	QString phraseButtonOk() override { return QCoreApplication::translate("MeetingUI", "OK"); }
	QString phraseButtonClose() override { return QCoreApplication::translate("MeetingUI", "Close"); }
	QString phraseButtonCancel() override { return QCoreApplication::translate("MeetingUI", "Cancel"); }
	QString phrasePanelCloseWarning() override { return QCoreApplication::translate("MeetingUI", "Warning"); }
	QString phrasePanelCloseUnsaved() override { return QCoreApplication::translate("MeetingUI", "Unsaved Changes"); }
	QString phrasePanelCloseAnyway() override { return QCoreApplication::translate("MeetingUI", "Close Anyway"); }
	QString phraseBotSharePhone() override { return QCoreApplication::translate("MeetingUI", "Share Phone Number"); }
	QString phraseBotSharePhoneTitle() override { return QCoreApplication::translate("MeetingUI", "Share Phone Number"); }
	QString phraseBotSharePhoneConfirm() override { return QCoreApplication::translate("MeetingUI", "Confirm Sharing"); }
	QString phraseBotAllowWrite() override { return QCoreApplication::translate("MeetingUI", "Allow Messages"); }
	QString phraseBotAllowWriteTitle() override { return QCoreApplication::translate("MeetingUI", "Permission Request"); }
	QString phraseBotAllowWriteConfirm() override { return QCoreApplication::translate("MeetingUI", "Confirm"); }
	QString phraseQuoteHeaderCopy() override { return QCoreApplication::translate("MeetingUI", "Copy Quote"); }
	QString phraseMinimize() override { return QCoreApplication::translate("MeetingUI", "Minimize"); }
	QString phraseMaximize() override { return QCoreApplication::translate("MeetingUI", "Maximize"); }
	QString phraseRestore() override { return QCoreApplication::translate("MeetingUI", "Restore"); }

private:
	int _touchCounter = 0;
};

} // namespace MeetingUI
