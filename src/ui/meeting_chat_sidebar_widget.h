#pragma once

#include <QtWidgets/QWidget>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPlainTextEdit>
#include <QtGui/QKeyEvent>
#include <QtGui/QImage>
#include <QtGui/QPixmap>
#include <QtGui/QDragEnterEvent>
#include <QtGui/QDragMoveEvent>
#include <QtGui/QDragLeaveEvent>
#include <QtGui/QDropEvent>
#include <QtCore/QMimeData>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QMap>
#include <QtWidgets/QProgressBar>
#include <vector>

namespace OpenMeeting {

enum class ChatMessageType {
    Text,
    Image,
    File
};

enum class MessageSendStatus {
    Sending,   // 发送中 / 上传中
    Sent,      // 发送成功 / 已送达 / 接收完成
    Failed,    // 失败
    Receiving  // 接收中 / 下载中
};

struct ChatMessageItem {
    QString id;
    QString senderIdentity;
    QString senderName;
    ChatMessageType type = ChatMessageType::Text;
    QString text;           // 文本内容或图片/文件的附带描述
    QString fileName;       // 文件名 (例如 screenshot.png, report.pdf)
    qint64 fileSize = 0;    // 文件大小 (字节)
    QByteArray fileData;    // 图片或文件的二进制数据
    QString localFilePath;  // 若为本地发出的文件，保留本地路径以便直接打开
    qint64 timestamp = 0;   // 毫秒时间戳
    qint64 seq = 0;         // 全局单调递增时序序号
    bool isMine = false;
    MessageSendStatus status = MessageSendStatus::Sent; // 发送状态 (远端接收默认为 Sent)
    int progress = 100;      // 传输进度 0~100
    QString errorMessage;   // 失败错误信息
};

// ----------------------------------------------------
// ChatInputEdit: 支持 Enter 发送与剪贴板图片粘贴 (Ctrl+V) 的多行输入框
// ----------------------------------------------------
class ChatInputEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit ChatInputEdit(QWidget *parent = nullptr);
    ~ChatInputEdit() override = default;

signals:
    void sendTriggered();
    void imagePasted(const QImage &image);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
};

// ----------------------------------------------------
// ChatBubbleWidget: 单条消息气泡组件（支持文本、图片预览与文件卡片）
// ----------------------------------------------------
class ChatBubbleWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatBubbleWidget(const ChatMessageItem &msg, QWidget *parent = nullptr);
    ~ChatBubbleWidget() override = default;

    static QString formatFileSize(qint64 bytes);

    QString messageId() const { return _msg.id; }
    void updateStatus(MessageSendStatus status, int progress = 100, const QString &errorMessage = QString());

    // 接收端多媒体流式更新
    void updateReceivingProgress(int progress);
    void completeReceivingMedia(const QByteArray &data);
    void failReceivingMedia(const QString &reason);

signals:
    void retryClicked(const QString &messageId);

private:
    void setupUi(const ChatMessageItem &msg);
    void setupTextBubble(QVBoxLayout *col, const ChatMessageItem &msg);
    void setupImageBubble(QVBoxLayout *col, const ChatMessageItem &msg);
    void setupFileBubble(QVBoxLayout *col, const ChatMessageItem &msg);

    static QColor avatarColor(const QString &seed);
    static void showImagePreview(const QImage &img, const QString &title);

    ChatMessageItem _msg;
    QLabel *_statusLabel = nullptr;
    QProgressBar *_progressBar = nullptr;
    QLabel *_progressLabel = nullptr;
    QPushButton *_retryBtn = nullptr;

    // 图片气泡动态组件
    QWidget *_imgContainer = nullptr;
    QLabel *_thumbLabel = nullptr;
    QPushButton *_imgPreviewBtn = nullptr;

    // 文件气泡动态操作按钮
    QPushButton *_actionBtn = nullptr;
};

// ----------------------------------------------------
// SendConfirmDialog: 拖拽发送文件/图片前的确认弹窗
// ----------------------------------------------------
class SendConfirmDialog : public QDialog {
    Q_OBJECT
public:
    explicit SendConfirmDialog(const QStringList &filePaths, QWidget *parent = nullptr);
    ~SendConfirmDialog() override = default;

private:
    void setupUi(const QStringList &filePaths);
};

// ----------------------------------------------------
// MeetingChatSidebarWidget: 会议聊天侧边栏主组件
// ----------------------------------------------------
class MeetingChatSidebarWidget : public QWidget {
    Q_OBJECT
public:
    explicit MeetingChatSidebarWidget(QWidget *parent = nullptr);
    ~MeetingChatSidebarWidget() override = default;

    void appendMessage(const ChatMessageItem &msg);
    void updateMessageStatus(const QString &messageId, MessageSendStatus status, int progress = 100, const QString &errorMessage = QString());
    
    // 接收端多媒体分片管理
    void startReceivingMedia(const QString &transferId, const QString &senderId, const QString &senderName,
                            const QString &mediaType, const QString &fileName, qint64 totalSize, qint64 seq = 0);
    void updateReceivingProgress(const QString &transferId, int progress);
    void completeReceivingMedia(const QString &transferId, const QString &mediaType, const QString &fileName, const QByteArray &data);
    void failReceivingMedia(const QString &transferId, const QString &reason);

    void clearMessages();
    int messageCount() const { return static_cast<int>(_messages.size()); }
    ChatMessageItem findMessage(const QString &messageId) const {
        if (_pendingMessages.contains(messageId)) {
            return _pendingMessages.value(messageId);
        }
        for (const auto &m : _messages) {
            if (m.id == messageId) return m;
        }
        return ChatMessageItem();
    }

signals:
    void messageSent(const QString &text);
    void imageSent(const QString &fileName, const QByteArray &data);
    void fileSent(const QString &fileName, const QByteArray &data);
    void retryRequested(const QString &messageId);
    void closeRequested();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private slots:
    void onSendClicked();
    void onTextChanged();
    void scrollToBottom();
    void onChooseImageClicked();
    void onChooseFileClicked();
    void onImagePasted(const QImage &image);

private:
    void setupUi();
    void handleDropMimeData(const QMimeData *mimeData);
    void handleDroppedFiles(const QStringList &filePaths);

    QLabel *_titleLabel = nullptr;
    QPushButton *_closeBtn = nullptr;

    QScrollArea *_scrollArea = nullptr;
    QWidget *_scrollContent = nullptr;
    QVBoxLayout *_messagesLayout = nullptr;
    QPushButton *_scrollToBottomBtn = nullptr;

    QWidget *_inputContainer = nullptr;
    ChatInputEdit *_inputEdit = nullptr;
    QPushButton *_imageBtn = nullptr;
    QPushButton *_fileBtn = nullptr;
    QPushButton *_sendBtn = nullptr;
    QLabel *_dropOverlay = nullptr;

    std::vector<ChatMessageItem> _messages;
    QMap<QString, ChatBubbleWidget*> _bubbleMap;
    QMap<QString, ChatMessageItem> _pendingMessages;
    bool _isAtBottom = true;
    bool _isDraggingOver = false;
};

} // namespace OpenMeeting
