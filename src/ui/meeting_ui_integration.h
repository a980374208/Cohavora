#pragma once

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

	QString phraseContextCopyText() override { return QString::fromUtf8("复制"); }
	QString phraseContextCopyEmail() override { return QString::fromUtf8("复制邮箱"); }
	QString phraseContextCopyLink() override { return QString::fromUtf8("复制链接"); }
	QString phraseContextCopySelected() override { return QString::fromUtf8("复制选中项"); }
	QString phraseFormattingTitle() override { return QString::fromUtf8("格式化"); }
	QString phraseFormattingLinkCreate() override { return QString::fromUtf8("创建链接"); }
	QString phraseFormattingLinkEdit() override { return QString::fromUtf8("编辑链接"); }
	QString phraseFormattingClear() override { return QString::fromUtf8("清除格式"); }
	QString phraseFormattingBold() override { return QString::fromUtf8("加粗"); }
	QString phraseFormattingItalic() override { return QString::fromUtf8("斜体"); }
	QString phraseFormattingUnderline() override { return QString::fromUtf8("下划线"); }
	QString phraseFormattingStrikeOut() override { return QString::fromUtf8("删除线"); }
	QString phraseFormattingBlockquote() override { return QString::fromUtf8("引用"); }
	QString phraseFormattingMonospace() override { return QString::fromUtf8("等宽"); }
	QString phraseFormattingSpoiler() override { return QString::fromUtf8("剧透隐藏"); }
	QString phraseFormattingDate() override { return QString::fromUtf8("日期"); }
	QString phraseButtonOk() override { return QString::fromUtf8("确定"); }
	QString phraseButtonClose() override { return QString::fromUtf8("关闭"); }
	QString phraseButtonCancel() override { return QString::fromUtf8("取消"); }
	QString phrasePanelCloseWarning() override { return QString::fromUtf8("警告"); }
	QString phrasePanelCloseUnsaved() override { return QString::fromUtf8("未保存"); }
	QString phrasePanelCloseAnyway() override { return QString::fromUtf8("仍然关闭"); }
	QString phraseBotSharePhone() override { return QString::fromUtf8("分享手机号"); }
	QString phraseBotSharePhoneTitle() override { return QString::fromUtf8("分享手机号"); }
	QString phraseBotSharePhoneConfirm() override { return QString::fromUtf8("确认分享"); }
	QString phraseBotAllowWrite() override { return QString::fromUtf8("允许写入"); }
	QString phraseBotAllowWriteTitle() override { return QString::fromUtf8("权限请求"); }
	QString phraseBotAllowWriteConfirm() override { return QString::fromUtf8("确认"); }
	QString phraseQuoteHeaderCopy() override { return QString::fromUtf8("复制引用"); }
	QString phraseMinimize() override { return QString::fromUtf8("最小化"); }
	QString phraseMaximize() override { return QString::fromUtf8("最大化"); }
	QString phraseRestore() override { return QString::fromUtf8("还原"); }

private:
	int _touchCounter = 0;
};

} // namespace MeetingUI
