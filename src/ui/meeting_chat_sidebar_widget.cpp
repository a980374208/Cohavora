#include "src/ui/meeting_chat_sidebar_widget.h"
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QGraphicsDropShadowEffect>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QDialog>
#include <QtCore/QTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QBuffer>
#include <QtCore/QUrl>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QDesktopServices>
#include <QtGui/QCursor>
#include <QtGui/QResizeEvent>

namespace OpenMeeting {

// ----------------------------------------------------
// ChatInputEdit 实现
// ----------------------------------------------------
ChatInputEdit::ChatInputEdit(QWidget *parent)
    : QPlainTextEdit(parent) {
    setPlaceholderText(QString::fromUtf8("发送消息... (Enter 发送, Shift+Enter 换行, Ctrl+V 粘贴截图)"));
    setFixedHeight(48);
    setStyleSheet(
        "QPlainTextEdit {"
        "  background-color: #242831;"
        "  border: 1px solid #363C4A;"
        "  border-radius: 6px;"
        "  color: #F3F4F6;"
        "  padding: 6px 8px;"
        "  font-size: 13px;"
        "  font-family: \"Microsoft YaHei\", sans-serif;"
        "}"
        "QPlainTextEdit:focus {"
        "  border: 1px solid #3B82F6;"
        "}"
    );
}

void ChatInputEdit::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (!(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
            emit sendTriggered();
            e->accept();
            return;
        }
    }
    QPlainTextEdit::keyPressEvent(e);
}

bool ChatInputEdit::canInsertFromMimeData(const QMimeData *source) const {
    if (!source) return false;
    return source->hasImage() || QPlainTextEdit::canInsertFromMimeData(source);
}

void ChatInputEdit::insertFromMimeData(const QMimeData *source) {
    if (source && source->hasImage()) {
        QImage img = qvariant_cast<QImage>(source->imageData());
        if (!img.isNull()) {
            emit imagePasted(img);
            return;
        }
    }
    QPlainTextEdit::insertFromMimeData(source);
}

// ----------------------------------------------------
// ChatBubbleWidget 实现
// ----------------------------------------------------
ChatBubbleWidget::ChatBubbleWidget(const ChatMessageItem &msg, QWidget *parent)
    : QWidget(parent), _msg(msg) {
    setupUi(msg);
}

QString ChatBubbleWidget::formatFileSize(qint64 bytes) {
    if (bytes < 1024) return QString::number(bytes) + " B";
    double kb = bytes / 1024.0;
    if (kb < 1024.0) return QString::number(kb, 'f', 1) + " KB";
    double mb = kb / 1024.0;
    return QString::number(mb, 'f', 1) + " MB";
}

QColor ChatBubbleWidget::avatarColor(const QString &seed) {
    static const std::vector<QColor> colors = {
        QColor("#4F46E5"), QColor("#059669"), QColor("#D97706"),
        QColor("#DC2626"), QColor("#7C3AED"), QColor("#0891B2"),
        QColor("#DB2777"), QColor("#2563EB")
    };
    uint h = qHash(seed);
    return colors[h % colors.size()];
}

void ChatBubbleWidget::showImagePreview(const QImage &img, const QString &title) {
    if (img.isNull()) return;

    auto *dlg = new QDialog();
    dlg->setWindowTitle(title.isEmpty() ? QString::fromUtf8("图片预览") : title);
    dlg->resize(std::min(1000, std::max(400, img.width() + 40)),
                std::min(800, std::max(300, img.height() + 80)));
    dlg->setStyleSheet("background-color: #12141A; color: #FFFFFF; font-family: \"Microsoft YaHei\";");

    auto *layout = new QVBoxLayout(dlg);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto *scroll = new QScrollArea(dlg);
    scroll->setStyleSheet("background-color: transparent; border: none;");
    scroll->setAlignment(Qt::AlignCenter);

    auto *imgLabel = new QLabel(scroll);
    imgLabel->setPixmap(QPixmap::fromImage(img).scaled(dlg->size() - QSize(60, 100), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    scroll->setWidget(imgLabel);
    layout->addWidget(scroll, 1);

    auto *bottomRow = new QHBoxLayout();
    bottomRow->addStretch();

    auto *saveBtn = new QPushButton(QString::fromUtf8("另存为..."), dlg);
    saveBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #2563EB;"
        "  color: #FFFFFF;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 6px 18px;"
        "  font-size: 12px;"
        "}"
        "QPushButton:hover { background-color: #1D4ED8; }"
    );
    QObject::connect(saveBtn, &QPushButton::clicked, [dlg, img, title]() {
        QString defaultName = title.isEmpty() ? "image.png" : title;
        QString savePath = QFileDialog::getSaveFileName(dlg, QString::fromUtf8("保存图片"), defaultName,
                                                        "PNG 图片 (*.png);;JPEG 图片 (*.jpg *.jpeg);;所有文件 (*.*)");
        if (!savePath.isEmpty()) {
            img.save(savePath);
            QMessageBox::information(dlg, QString::fromUtf8("保存成功"), QString::fromUtf8("图片已成功保存到:\n%1").arg(savePath));
        }
    });
    bottomRow->addWidget(saveBtn);

    auto *closeBtn = new QPushButton(QString::fromUtf8("关闭"), dlg);
    closeBtn->setStyleSheet(
        "QPushButton {"
        "  background-color: #374151;"
        "  color: #E5E7EB;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 6px 18px;"
        "  font-size: 12px;"
        "}"
        "QPushButton:hover { background-color: #4B5563; }"
    );
    QObject::connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);
    bottomRow->addWidget(closeBtn);

    layout->addLayout(bottomRow);

    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void ChatBubbleWidget::setupUi(const ChatMessageItem &msg) {
    auto *mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(12, 4, 12, 4);
    mainLayout->setSpacing(8);

    QString timeStr;
    if (msg.timestamp > 0) {
        timeStr = QDateTime::fromMSecsSinceEpoch(msg.timestamp).toString("HH:mm");
    } else {
        timeStr = QDateTime::currentDateTime().toString("HH:mm");
    }

    if (msg.isMine) {
        // 自己发的消息：靠右排列
        mainLayout->addStretch();

        auto *contentCol = new QVBoxLayout();
        contentCol->setContentsMargins(0, 0, 0, 0);
        contentCol->setSpacing(2);
        contentCol->setAlignment(Qt::AlignRight);

        if (msg.type == ChatMessageType::Image) {
            setupImageBubble(contentCol, msg);
        } else if (msg.type == ChatMessageType::File) {
            setupFileBubble(contentCol, msg);
        } else {
            setupTextBubble(contentCol, msg);
        }

        auto *bottomRow = new QHBoxLayout();
        bottomRow->setContentsMargins(0, 0, 0, 0);
        bottomRow->setSpacing(4);
        bottomRow->addStretch();

        _retryBtn = new QPushButton(QString::fromUtf8("重试"), this);
        _retryBtn->setCursor(Qt::PointingHandCursor);
        _retryBtn->setStyleSheet(
            "QPushButton { background: transparent; color: #EF4444; border: none; font-size: 10px; font-weight: bold; padding: 0px 2px; }"
            "QPushButton:hover { text-decoration: underline; color: #F87171; }"
        );
        _retryBtn->hide();
        connect(_retryBtn, &QPushButton::clicked, this, [this]() {
            emit retryClicked(_msg.id);
        });
        bottomRow->addWidget(_retryBtn);

        auto *timeLabel = new QLabel(timeStr, this);
        timeLabel->setStyleSheet("color: #6B7280; font-size: 10px; font-family: \"Microsoft YaHei\";");
        bottomRow->addWidget(timeLabel);

        _statusLabel = new QLabel(this);
        _statusLabel->setStyleSheet("font-size: 11px; font-family: \"Microsoft YaHei\";");
        bottomRow->addWidget(_statusLabel);

        contentCol->addLayout(bottomRow);
        mainLayout->addLayout(contentCol);

        updateStatus(msg.status, msg.progress, msg.errorMessage);
    } else {
        // 远端消息：靠左排列，带彩色圆形头像与发送者昵称
        auto *avatarLabel = new QLabel(this);
        avatarLabel->setFixedSize(30, 30);
        QColor bgColor = avatarColor(msg.senderIdentity.isEmpty() ? msg.senderName : msg.senderIdentity);
        QPixmap pix(30, 30);
        pix.fill(Qt::transparent);
        {
            QPainter p(&pix);
            p.setRenderHint(QPainter::Antialiasing);
            p.setBrush(bgColor);
            p.setPen(Qt::NoPen);
            p.drawEllipse(0, 0, 30, 30);

            QString initial = msg.senderName.trimmed();
            if (initial.isEmpty()) initial = msg.senderIdentity;
            if (!initial.isEmpty()) initial = initial.left(1).toUpper();
            else initial = "?";

            p.setPen(Qt::white);
            QFont f("Microsoft YaHei", 10, QFont::Bold);
            p.setFont(f);
            p.drawText(QRect(0, 0, 30, 30), Qt::AlignCenter, initial);
        }
        avatarLabel->setPixmap(pix);
        mainLayout->addWidget(avatarLabel, 0, Qt::AlignTop);

        auto *contentCol = new QVBoxLayout();
        contentCol->setContentsMargins(0, 0, 0, 0);
        contentCol->setSpacing(3);

        auto *metaLayout = new QHBoxLayout();
        metaLayout->setContentsMargins(0, 0, 0, 0);
        metaLayout->setSpacing(6);

        auto *nameLabel = new QLabel(msg.senderName.isEmpty() ? msg.senderIdentity : msg.senderName, this);
        nameLabel->setStyleSheet("color: #9CA3AF; font-size: 11px; font-weight: 500; font-family: \"Microsoft YaHei\";");
        metaLayout->addWidget(nameLabel);

        auto *timeLabel = new QLabel(timeStr, this);
        timeLabel->setStyleSheet("color: #6B7280; font-size: 10px; font-family: \"Microsoft YaHei\";");
        metaLayout->addWidget(timeLabel);
        metaLayout->addStretch();
        contentCol->addLayout(metaLayout);

        if (msg.type == ChatMessageType::Image) {
            setupImageBubble(contentCol, msg);
        } else if (msg.type == ChatMessageType::File) {
            setupFileBubble(contentCol, msg);
        } else {
            setupTextBubble(contentCol, msg);
        }

        mainLayout->addLayout(contentCol);
        mainLayout->addStretch();
    }
}

void ChatBubbleWidget::setupTextBubble(QVBoxLayout *col, const ChatMessageItem &msg) {
    auto *bubble = new QWidget(this);
    bubble->setObjectName(msg.isMine ? "MyTextBubble" : "OtherTextBubble");
    bubble->setStyleSheet(
        msg.isMine
        ? "QWidget#MyTextBubble { background-color: #2563EB; border-radius: 8px; }"
        : "QWidget#OtherTextBubble { background-color: #242831; border: 1px solid #363C4A; border-radius: 8px; }"
    );

    auto *bubbleLayout = new QVBoxLayout(bubble);
    bubbleLayout->setContentsMargins(10, 8, 10, 8);
    bubbleLayout->setSpacing(0);

    auto *textLabel = new QLabel(msg.text, bubble);
    textLabel->setWordWrap(true);
    textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textLabel->setStyleSheet(
        msg.isMine
        ? "color: #FFFFFF; font-size: 13px; font-family: \"Microsoft YaHei\"; line-height: 1.4;"
        : "color: #F3F4F6; font-size: 13px; font-family: \"Microsoft YaHei\"; line-height: 1.4;"
    );
    textLabel->setMaximumWidth(230);

    bubbleLayout->addWidget(textLabel);
    col->addWidget(bubble);
}

void ChatBubbleWidget::setupImageBubble(QVBoxLayout *col, const ChatMessageItem &msg) {
    QImage img;
    if (!msg.fileData.isEmpty()) {
        img.loadFromData(msg.fileData);
    } else if (!msg.localFilePath.isEmpty()) {
        img.load(msg.localFilePath);
    }

    if (img.isNull() && msg.status != MessageSendStatus::Receiving) {
        setupTextBubble(col, msg);
        return;
    }

    _imgContainer = new QWidget(this);
    _imgContainer->setCursor(Qt::PointingHandCursor);
    _imgContainer->setObjectName(msg.isMine ? "MyImgContainer" : "OtherImgContainer");

    auto *imgLayout = new QVBoxLayout(_imgContainer);
    imgLayout->setContentsMargins(4, 4, 4, 4);
    imgLayout->setSpacing(4);

    _thumbLabel = new QLabel(_imgContainer);
    _thumbLabel->setAlignment(Qt::AlignCenter);

    if (msg.status == MessageSendStatus::Receiving) {
        _imgContainer->setStyleSheet(
            "QWidget#OtherImgContainer { background-color: #1F242D; border: 1px dashed #3B4252; border-radius: 8px; padding: 4px; }"
        );
        _thumbLabel->setFixedSize(210, 130);

        QPixmap placePix(210, 130);
        placePix.fill(QColor(26, 30, 39));
        {
            QPainter p(&placePix);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QColor(156, 163, 175));
            QFont f("Microsoft YaHei", 24);
            p.setFont(f);
            p.drawText(QRect(0, 15, 210, 50), Qt::AlignCenter, QString::fromUtf8("🖼️"));

            QFont f2("Microsoft YaHei", 10);
            p.setFont(f2);
            p.setPen(QColor(156, 163, 175));
            p.drawText(QRect(0, 75, 210, 30), Qt::AlignCenter, QString::fromUtf8("正在接收图片..."));
        }
        _thumbLabel->setPixmap(placePix);
    } else {
        _imgContainer->setStyleSheet(
            msg.isMine
            ? "QWidget#MyImgContainer { background-color: #1D4ED8; border-radius: 8px; padding: 4px; }"
            : "QWidget#OtherImgContainer { background-color: #242831; border: 1px solid #363C4A; border-radius: 8px; padding: 4px; }"
        );

        // 缩略图保真等比缩放
        QImage thumb = img.scaled(210, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmap roundedThumb(thumb.size());
        roundedThumb.fill(Qt::transparent);
        {
            QPainter p(&roundedThumb);
            p.setRenderHint(QPainter::Antialiasing);
            QPainterPath path;
            path.addRoundedRect(0, 0, thumb.width(), thumb.height(), 6, 6);
            p.setClipPath(path);
            p.drawImage(0, 0, thumb);
        }
        _thumbLabel->setPixmap(roundedThumb);
    }
    imgLayout->addWidget(_thumbLabel);

    if (!msg.fileName.isEmpty()) {
        auto *fnLabel = new QLabel(msg.fileName, _imgContainer);
        fnLabel->setStyleSheet("color: rgba(255, 255, 255, 0.7); font-size: 11px; font-family: \"Microsoft YaHei\";");
        fnLabel->setMaximumWidth(210);
        imgLayout->addWidget(fnLabel);
    }

    if (msg.isMine || msg.status == MessageSendStatus::Receiving) {
        _statusLabel = new QLabel(_imgContainer);
        if (msg.status == MessageSendStatus::Receiving) {
            _statusLabel->setText(QString::fromUtf8("📥 接收中 %1%").arg(msg.progress));
            _statusLabel->setStyleSheet("color: #60A5FA; font-size: 10px; font-family: \"Microsoft YaHei\";");
        } else {
            _statusLabel->hide();
        }
        imgLayout->addWidget(_statusLabel);

        _progressBar = new QProgressBar(_imgContainer);
        _progressBar->setFixedHeight(3);
        _progressBar->setTextVisible(false);
        _progressBar->setRange(0, 100);
        _progressBar->setValue(msg.progress);
        _progressBar->setStyleSheet(
            "QProgressBar { background-color: rgba(255, 255, 255, 0.2); border: none; border-radius: 1px; }"
            "QProgressBar::chunk { background-color: #60A5FA; border-radius: 1px; }"
        );
        if (msg.status != MessageSendStatus::Sending && msg.status != MessageSendStatus::Receiving) {
            _progressBar->hide();
        }
        imgLayout->addWidget(_progressBar);
    }

    col->addWidget(_imgContainer);

    // 点击弹出大图预览按钮
    _imgPreviewBtn = new QPushButton(_imgContainer);
    _imgPreviewBtn->setStyleSheet("background: transparent; border: none;");
    _imgPreviewBtn->setGeometry(0, 0, 220, 180);
    _imgPreviewBtn->raise();

    if (msg.status == MessageSendStatus::Receiving) {
        _imgPreviewBtn->setEnabled(false);
    } else {
        QString title = msg.fileName.isEmpty() ? QString::fromUtf8("图片预览") : msg.fileName;
        connect(_imgPreviewBtn, &QPushButton::clicked, [img, title]() {
            showImagePreview(img, title);
        });
    }
}

void ChatBubbleWidget::setupFileBubble(QVBoxLayout *col, const ChatMessageItem &msg) {
    auto *card = new QWidget(this);
    card->setObjectName(msg.isMine ? "MyFileCard" : "OtherFileCard");
    card->setFixedWidth(230);
    card->setStyleSheet(
        msg.isMine
        ? "QWidget#MyFileCard { background-color: #2563EB; border-radius: 8px; }"
        : "QWidget#OtherFileCard { background-color: #242831; border: 1px solid #363C4A; border-radius: 8px; }"
    );

    auto *cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(10, 8, 10, 8);
    cardLayout->setSpacing(8);

    // 文件类型图标
    QString ext = QFileInfo(msg.fileName).suffix().toUpper();
    if (ext.isEmpty()) ext = "FILE";
    QColor iconColor = "#4B5563";
    if (ext == "PDF") iconColor = "#DC2626";
    else if (ext == "DOC" || ext == "DOCX") iconColor = "#2563EB";
    else if (ext == "XLS" || ext == "XLSX") iconColor = "#059669";
    else if (ext == "ZIP" || ext == "RAR" || ext == "7Z") iconColor = "#D97706";
    else if (ext == "TXT") iconColor = "#4B5563";

    auto *iconLabel = new QLabel(card);
    iconLabel->setFixedSize(36, 36);
    QPixmap iconPix(36, 36);
    iconPix.fill(Qt::transparent);
    {
        QPainter p(&iconPix);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(iconColor);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(0, 0, 36, 36, 6, 6);

        p.setPen(Qt::white);
        QFont f("Microsoft YaHei", 9, QFont::Bold);
        p.setFont(f);
        p.drawText(QRect(0, 0, 36, 36), Qt::AlignCenter, ext.left(4));
    }
    iconLabel->setPixmap(iconPix);
    cardLayout->addWidget(iconLabel);

    // 文件信息列
    auto *infoCol = new QVBoxLayout();
    infoCol->setContentsMargins(0, 0, 0, 0);
    infoCol->setSpacing(2);

    auto *nameLabel = new QLabel(msg.fileName.isEmpty() ? QString::fromUtf8("未命名文件") : msg.fileName, card);
    nameLabel->setStyleSheet("color: #FFFFFF; font-size: 12px; font-weight: bold; font-family: \"Microsoft YaHei\";");
    nameLabel->setMaximumWidth(130);
    nameLabel->setToolTip(msg.fileName);
    infoCol->addWidget(nameLabel);

    QString sizeStr = formatFileSize(msg.fileSize > 0 ? msg.fileSize : msg.fileData.size());
    auto *sizeLabel = new QLabel(sizeStr, card);
    sizeLabel->setStyleSheet("color: rgba(255, 255, 255, 0.65); font-size: 10px; font-family: \"Microsoft YaHei\";");
    infoCol->addWidget(sizeLabel);

    if (msg.isMine || msg.status == MessageSendStatus::Receiving) {
        _statusLabel = new QLabel(card);
        if (msg.status == MessageSendStatus::Receiving) {
            _statusLabel->setText(QString::fromUtf8("📥 接收中 %1%").arg(msg.progress));
            _statusLabel->setStyleSheet("color: #60A5FA; font-size: 10px; font-family: \"Microsoft YaHei\";");
        } else {
            _statusLabel->hide();
        }
        infoCol->addWidget(_statusLabel);

        _progressBar = new QProgressBar(card);
        _progressBar->setFixedHeight(3);
        _progressBar->setTextVisible(false);
        _progressBar->setRange(0, 100);
        _progressBar->setValue(msg.progress);
        _progressBar->setStyleSheet(
            "QProgressBar { background-color: rgba(255, 255, 255, 0.2); border: none; border-radius: 1px; }"
            "QProgressBar::chunk { background-color: #60A5FA; border-radius: 1px; }"
        );
        if (msg.status != MessageSendStatus::Sending && msg.status != MessageSendStatus::Receiving) {
            _progressBar->hide();
        }
        infoCol->addWidget(_progressBar);
    }

    cardLayout->addLayout(infoCol);

    // 下载/保存/打开按钮
    _actionBtn = new QPushButton(card);
    _actionBtn->setFixedSize(28, 28);
    _actionBtn->setCursor(Qt::PointingHandCursor);
    if (msg.status == MessageSendStatus::Receiving) {
        _actionBtn->setEnabled(false);
        _actionBtn->setStyleSheet(
            "QPushButton {"
            "  background-color: rgba(255, 255, 255, 0.05);"
            "  color: #6B7280;"
            "  border-radius: 14px;"
            "  border: none;"
            "  font-size: 12px;"
            "}"
        );
        _actionBtn->setText("💾");
        _actionBtn->setToolTip(QString::fromUtf8("正在接收文件..."));
    } else {
        _actionBtn->setStyleSheet(
            "QPushButton {"
            "  background-color: rgba(255, 255, 255, 0.15);"
            "  color: #FFFFFF;"
            "  border-radius: 14px;"
            "  border: none;"
            "  font-size: 12px;"
            "}"
            "QPushButton:hover {"
            "  background-color: rgba(255, 255, 255, 0.3);"
            "}"
        );
        _actionBtn->setText(msg.isMine && !msg.localFilePath.isEmpty() ? "📂" : "💾");
        _actionBtn->setToolTip(msg.isMine && !msg.localFilePath.isEmpty() ? QString::fromUtf8("打开文件") : QString::fromUtf8("另存为..."));
    }

    connect(_actionBtn, &QPushButton::clicked, [this, card]() {
        QString localPath = _msg.localFilePath;
        if (!localPath.isEmpty() && QFileInfo::exists(localPath)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
            return;
        }
        QByteArray data = _msg.fileData;
        if (data.isEmpty()) {
            QMessageBox::warning(card, QString::fromUtf8("文件不可用"), QString::fromUtf8("该文件未包含本地数据或传输中断。"));
            return;
        }
        QString fName = _msg.fileName;
        QString savePath = QFileDialog::getSaveFileName(card, QString::fromUtf8("另存为文件"), fName, "所有文件 (*.*)");
        if (!savePath.isEmpty()) {
            QFile file(savePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(data);
                file.close();
                auto res = QMessageBox::information(card, QString::fromUtf8("保存成功"),
                                                     QString::fromUtf8("文件已保存至:\n%1\n\n是否立即打开？").arg(savePath),
                                                     QMessageBox::Yes | QMessageBox::No);
                if (res == QMessageBox::Yes) {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(savePath));
                }
            } else {
                QMessageBox::critical(card, QString::fromUtf8("保存失败"), QString::fromUtf8("无法写入目标路径文件。"));
            }
        }
    });

    cardLayout->addWidget(_actionBtn);
    col->addWidget(card);
}

void ChatBubbleWidget::updateStatus(MessageSendStatus status, int progress, const QString &errorMessage) {
    _msg.status = status;
    _msg.progress = progress;
    _msg.errorMessage = errorMessage;

    if (!_statusLabel) return;

    if (status == MessageSendStatus::Sending) {
        if (_msg.type == ChatMessageType::Text) {
            _statusLabel->setText(QString::fromUtf8("⏳"));
            _statusLabel->setStyleSheet("color: #9CA3AF; font-size: 10px;");
        } else {
            _statusLabel->setText(QString::fromUtf8("⏳ %1%").arg(progress));
            _statusLabel->setStyleSheet("color: #93C5FD; font-size: 10px; font-weight: 500; font-family: \"Microsoft YaHei\";");
        }
        _statusLabel->setToolTip(QString::fromUtf8("正在发送..."));
        if (_progressBar) {
            _progressBar->setValue(progress);
            _progressBar->show();
        }
        if (_retryBtn) _retryBtn->hide();
    } else if (status == MessageSendStatus::Sent) {
        _statusLabel->setText(QString::fromUtf8("✓"));
        _statusLabel->setStyleSheet("color: #60A5FA; font-size: 11px; font-weight: bold; font-family: \"Microsoft YaHei\";");
        _statusLabel->setToolTip(QString::fromUtf8("已发送"));
        if (_progressBar) _progressBar->hide();
        if (_retryBtn) _retryBtn->hide();
    } else if (status == MessageSendStatus::Failed) {
        _statusLabel->setText(QString::fromUtf8("⚠️"));
        _statusLabel->setStyleSheet("color: #EF4444; font-size: 11px; font-family: \"Microsoft YaHei\";");
        _statusLabel->setToolTip(errorMessage.isEmpty() ? QString::fromUtf8("发送失败，点击重试") : errorMessage);
        if (_progressBar) _progressBar->hide();
        if (_retryBtn) _retryBtn->show();
    }
}

void ChatBubbleWidget::updateReceivingProgress(int progress) {
    _msg.progress = progress;
    if (_progressBar) {
        _progressBar->setValue(progress);
        _progressBar->show();
    }
    if (_statusLabel) {
        _statusLabel->setText(QString::fromUtf8("📥 接收中 %1%").arg(progress));
        _statusLabel->setStyleSheet("color: #60A5FA; font-size: 10px; font-family: \"Microsoft YaHei\";");
        _statusLabel->show();
    }
}

void ChatBubbleWidget::completeReceivingMedia(const QByteArray &data) {
    _msg.fileData = data;
    _msg.fileSize = data.size();
    _msg.status = MessageSendStatus::Sent;
    _msg.progress = 100;

    if (_progressBar) {
        _progressBar->hide();
    }
    if (_statusLabel) {
        _statusLabel->hide();
    }

    if (_msg.type == ChatMessageType::Image) {
        QImage img;
        img.loadFromData(data);
        if (!img.isNull() && _thumbLabel) {
            QImage thumb = img.scaled(210, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            QPixmap roundedThumb(thumb.size());
            roundedThumb.fill(Qt::transparent);
            {
                QPainter p(&roundedThumb);
                p.setRenderHint(QPainter::Antialiasing);
                QPainterPath path;
                path.addRoundedRect(0, 0, thumb.width(), thumb.height(), 6, 6);
                p.setClipPath(path);
                p.drawImage(0, 0, thumb);
            }
            _thumbLabel->setPixmap(roundedThumb);
            _thumbLabel->setFixedSize(roundedThumb.size());

            if (_imgContainer) {
                _imgContainer->setStyleSheet("QWidget#OtherImgContainer { background-color: #242831; border: 1px solid #363C4A; border-radius: 8px; padding: 4px; }");
            }

            if (_imgPreviewBtn) {
                _imgPreviewBtn->setEnabled(true);
                QString title = _msg.fileName.isEmpty() ? QString::fromUtf8("图片预览") : _msg.fileName;
                disconnect(_imgPreviewBtn, &QPushButton::clicked, nullptr, nullptr);
                connect(_imgPreviewBtn, &QPushButton::clicked, [img, title]() {
                    showImagePreview(img, title);
                });
            }
        }
    } else if (_msg.type == ChatMessageType::File) {
        if (_actionBtn) {
            _actionBtn->setEnabled(true);
            _actionBtn->setStyleSheet(
                "QPushButton {"
                "  background-color: rgba(255, 255, 255, 0.15);"
                "  color: #FFFFFF;"
                "  border-radius: 14px;"
                "  border: none;"
                "  font-size: 12px;"
                "}"
                "QPushButton:hover {"
                "  background-color: rgba(255, 255, 255, 0.3);"
                "}"
            );
            _actionBtn->setText("💾");
            _actionBtn->setToolTip(QString::fromUtf8("另存为..."));
        }
    }
}

void ChatBubbleWidget::failReceivingMedia(const QString &reason) {
    _msg.status = MessageSendStatus::Failed;
    if (_progressBar) {
        _progressBar->hide();
    }
    if (_statusLabel) {
        _statusLabel->setText(QString::fromUtf8("⚠️ %1").arg(reason.isEmpty() ? QString::fromUtf8("接收中断") : reason));
        _statusLabel->setStyleSheet("color: #EF4444; font-size: 10px; font-weight: bold; font-family: \"Microsoft YaHei\";");
        _statusLabel->show();
    }
}

// ----------------------------------------------------
// SendConfirmDialog 实现
// ----------------------------------------------------
SendConfirmDialog::SendConfirmDialog(const QStringList &filePaths, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(QString::fromUtf8("发送确认"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setStyleSheet(
        "QDialog {"
        "  background-color: #1A1D24;"
        "  color: #FFFFFF;"
        "  font-family: \"Microsoft YaHei\", sans-serif;"
        "}"
        "QLabel { color: #E5E7EB; font-family: \"Microsoft YaHei\", sans-serif; }"
        "QPushButton#CancelBtn {"
        "  background-color: #374151;"
        "  color: #E5E7EB;"
        "  border: 1px solid #4B5563;"
        "  border-radius: 6px;"
        "  padding: 6px 18px;"
        "  font-size: 13px;"
        "}"
        "QPushButton#CancelBtn:hover { background-color: #4B5563; }"
        "QPushButton#SendBtn {"
        "  background-color: #2563EB;"
        "  color: #FFFFFF;"
        "  border: none;"
        "  border-radius: 6px;"
        "  padding: 6px 22px;"
        "  font-size: 13px;"
        "  font-weight: bold;"
        "}"
        "QPushButton#SendBtn:hover { background-color: #1D4ED8; }"
        "QPushButton#SendBtn:disabled {"
        "  background-color: #374151;"
        "  color: #6B7280;"
        "}"
    );
    setupUi(filePaths);
}

void SendConfirmDialog::setupUi(const QStringList &filePaths) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 18);
    layout->setSpacing(14);

    auto *titleLabel = new QLabel(QString::fromUtf8("📤 确认发送以下文件到会议聊天？"), this);
    titleLabel->setStyleSheet("font-size: 15px; font-weight: bold; color: #F9FAFB;");
    layout->addWidget(titleLabel);

    bool hasOversize = false;
    qint64 totalSize = 0;
    static const QStringList imgExts = {"png", "jpg", "jpeg", "bmp", "gif", "webp"};

    if (filePaths.size() == 1) {
        QString path = filePaths.first();
        QFileInfo fi(path);
        totalSize = fi.size();
        if (totalSize > 15 * 1024 * 1024) {
            hasOversize = true;
        }

        QString ext = fi.suffix().toLower();
        bool isImg = imgExts.contains(ext);

        if (isImg) {
            QImage img(path);
            if (!img.isNull()) {
                auto *imgCard = new QWidget(this);
                imgCard->setStyleSheet("background-color: #12141A; border: 1px solid #2B303C; border-radius: 8px;");
                auto *cardLayout = new QVBoxLayout(imgCard);
                cardLayout->setContentsMargins(12, 12, 12, 12);
                cardLayout->setSpacing(8);

                auto *previewLabel = new QLabel(imgCard);
                previewLabel->setAlignment(Qt::AlignCenter);
                QPixmap pix = QPixmap::fromImage(img);
                previewLabel->setPixmap(pix.scaled(320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                cardLayout->addWidget(previewLabel);

                auto *infoLabel = new QLabel(
                    QString::fromUtf8("%1\n%2 × %3  •  %4")
                        .arg(fi.fileName())
                        .arg(img.width())
                        .arg(img.height())
                        .arg(ChatBubbleWidget::formatFileSize(fi.size())),
                    imgCard
                );
                infoLabel->setAlignment(Qt::AlignCenter);
                infoLabel->setStyleSheet("color: #9CA3AF; font-size: 12px; line-height: 1.4;");
                cardLayout->addWidget(infoLabel);

                layout->addWidget(imgCard);
            } else {
                isImg = false;
            }
        }

        if (!isImg) {
            auto *fileCard = new QWidget(this);
            fileCard->setStyleSheet("background-color: #242831; border: 1px solid #363C4A; border-radius: 8px;");
            auto *cardLayout = new QHBoxLayout(fileCard);
            cardLayout->setContentsMargins(14, 14, 14, 14);
            cardLayout->setSpacing(12);

            auto *badge = new QLabel(ext.isEmpty() ? "FILE" : ext.left(4).toUpper(), fileCard);
            badge->setFixedSize(44, 44);
            badge->setAlignment(Qt::AlignCenter);
            QString badgeColor = "#2563EB";
            if (ext == "pdf") badgeColor = "#DC2626";
            else if (ext == "zip" || ext == "rar" || ext == "7z") badgeColor = "#D97706";
            else if (ext == "doc" || ext == "docx") badgeColor = "#2563EB";
            else if (ext == "xls" || ext == "xlsx") badgeColor = "#059669";
            else if (ext == "txt" || ext == "log" || ext == "md") badgeColor = "#4B5563";
            badge->setStyleSheet(QString(
                "background-color: %1; color: #FFFFFF; font-size: 11px; font-weight: bold; border-radius: 6px;"
            ).arg(badgeColor));
            cardLayout->addWidget(badge);

            auto *textCol = new QVBoxLayout();
            textCol->setSpacing(4);
            auto *nameLabel = new QLabel(fi.fileName(), fileCard);
            nameLabel->setStyleSheet("color: #F3F4F6; font-size: 13px; font-weight: bold;");
            textCol->addWidget(nameLabel);

            auto *sizeLabel = new QLabel(ChatBubbleWidget::formatFileSize(fi.size()), fileCard);
            sizeLabel->setStyleSheet("color: #9CA3AF; font-size: 12px;");
            textCol->addWidget(sizeLabel);

            cardLayout->addLayout(textCol, 1);
            layout->addWidget(fileCard);
        }
    } else {
        auto *scroll = new QScrollArea(this);
        scroll->setStyleSheet("background-color: transparent; border: 1px solid #2B303C; border-radius: 6px;");
        scroll->setWidgetResizable(true);
        scroll->setMaximumHeight(220);

        auto *container = new QWidget();
        container->setStyleSheet("background-color: transparent;");
        auto *listLayout = new QVBoxLayout(container);
        listLayout->setContentsMargins(8, 8, 8, 8);
        listLayout->setSpacing(6);

        for (const QString &path : filePaths) {
            QFileInfo fi(path);
            totalSize += fi.size();
            bool isItemOversize = (fi.size() > 15 * 1024 * 1024);
            if (isItemOversize) {
                hasOversize = true;
            }

            QString ext = fi.suffix().toLower();
            auto *itemRow = new QWidget(container);
            itemRow->setStyleSheet(isItemOversize
                ? "background-color: rgba(239, 68, 68, 0.15); border: 1px solid #EF4444; border-radius: 4px; padding: 4px 8px;"
                : "background-color: #242831; border-radius: 4px; padding: 4px 8px;");
            auto *rowLayout = new QHBoxLayout(itemRow);
            rowLayout->setContentsMargins(6, 4, 6, 4);
            rowLayout->setSpacing(8);

            QString iconText = isItemOversize ? "❌" : (imgExts.contains(ext) ? "🖼️" : "📄");
            auto *iconLbl = new QLabel(iconText, itemRow);
            rowLayout->addWidget(iconLbl);

            auto *nameLbl = new QLabel(fi.fileName(), itemRow);
            nameLbl->setStyleSheet(isItemOversize
                ? "color: #FCA5A5; font-size: 12px; font-weight: bold;"
                : "color: #F3F4F6; font-size: 12px;");
            rowLayout->addWidget(nameLbl, 1);

            QString sizeText = isItemOversize
                ? QString::fromUtf8("%1 (超限)").arg(ChatBubbleWidget::formatFileSize(fi.size()))
                : ChatBubbleWidget::formatFileSize(fi.size());
            auto *sizeLbl = new QLabel(sizeText, itemRow);
            sizeLbl->setStyleSheet(isItemOversize
                ? "color: #EF4444; font-size: 11px; font-weight: bold;"
                : "color: #9CA3AF; font-size: 11px;");
            rowLayout->addWidget(sizeLbl);

            listLayout->addWidget(itemRow);
        }
        scroll->setWidget(container);
        layout->addWidget(scroll);

        auto *summaryLbl = new QLabel(
            QString::fromUtf8("共 %1 个文件，总大小: %2")
                .arg(filePaths.size())
                .arg(ChatBubbleWidget::formatFileSize(totalSize)),
            this
        );
        summaryLbl->setStyleSheet("color: #9CA3AF; font-size: 12px;");
        layout->addWidget(summaryLbl);
    }

    if (hasOversize) {
        QString warnText = (filePaths.size() == 1)
            ? QString::fromUtf8("❌ 注意：文件大于 15MB 限制（当前大小：%1），系统已禁止发送！").arg(ChatBubbleWidget::formatFileSize(totalSize))
            : QString::fromUtf8("❌ 注意：列表中包含大于 15MB 的文件（单文件限制 15MB），系统已禁止发送！");
        auto *warnLbl = new QLabel(warnText, this);
        warnLbl->setWordWrap(true);
        warnLbl->setStyleSheet("color: #EF4444; font-size: 12px; font-weight: bold; line-height: 1.3;");
        layout->addWidget(warnLbl);
    }

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(10);
    btnRow->addStretch();

    auto *cancelBtn = new QPushButton(QString::fromUtf8("取消"), this);
    cancelBtn->setObjectName("CancelBtn");
    cancelBtn->setCursor(Qt::PointingHandCursor);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);

    auto *sendBtn = new QPushButton(QString::fromUtf8("确认发送"), this);
    sendBtn->setObjectName("SendBtn");
    sendBtn->setCursor(Qt::PointingHandCursor);
    if (hasOversize) {
        sendBtn->setEnabled(false);
        sendBtn->setToolTip(QString::fromUtf8("存在超过 15MB 的文件，禁止发送"));
        sendBtn->setDefault(false);
    } else {
        sendBtn->setDefault(true);
    }
    connect(sendBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(sendBtn);

    layout->addLayout(btnRow);

    setMinimumWidth(380);
    adjustSize();
}

// ----------------------------------------------------
// MeetingChatSidebarWidget 实现
// ----------------------------------------------------
MeetingChatSidebarWidget::MeetingChatSidebarWidget(QWidget *parent)
    : QWidget(parent) {
    setupUi();
}

void MeetingChatSidebarWidget::setupUi() {
    setFixedWidth(340);
    setStyleSheet(
        "QWidget#ChatSidebar {"
        "  background-color: #1A1D24;"
        "  border-left: 1px solid #2B303C;"
        "}"
        "QLabel#ChatTitle {"
        "  color: #F3F4F6;"
        "  font-size: 14px;"
        "  font-weight: bold;"
        "  font-family: \"Microsoft YaHei\";"
        "}"
        "QPushButton#CloseBtn {"
        "  background: transparent;"
        "  color: #9CA3AF;"
        "  font-size: 16px;"
        "  border: none;"
        "  border-radius: 4px;"
        "  min-width: 24px;"
        "  max-width: 24px;"
        "  min-height: 24px;"
        "  max-height: 24px;"
        "}"
        "QPushButton#CloseBtn:hover {"
        "  background-color: rgba(255, 255, 255, 0.1);"
        "  color: #FFFFFF;"
        "}"
        "QScrollArea#MessageScrollArea {"
        "  background-color: transparent;"
        "  border: none;"
        "}"
        "QScrollBar:vertical {"
        "  border: none;"
        "  background: transparent;"
        "  width: 6px;"
        "  margin: 0px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: #4B5563;"
        "  min-height: 20px;"
        "  border-radius: 3px;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "  height: 0px;"
        "}"
        "QWidget#BottomInputPanel {"
        "  background-color: #171A20;"
        "  border-top: 1px solid #2B303C;"
        "}"
        "QPushButton#MediaBtn {"
        "  background-color: transparent;"
        "  color: #9CA3AF;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 4px 8px;"
        "  font-size: 13px;"
        "}"
        "QPushButton#MediaBtn:hover {"
        "  background-color: rgba(255, 255, 255, 0.08);"
        "  color: #F3F4F6;"
        "}"
        "QPushButton#SendBtn {"
        "  background-color: #2563EB;"
        "  color: #FFFFFF;"
        "  border: none;"
        "  border-radius: 4px;"
        "  padding: 6px 14px;"
        "  font-size: 12px;"
        "  font-weight: 500;"
        "  font-family: \"Microsoft YaHei\";"
        "}"
        "QPushButton#SendBtn:hover {"
        "  background-color: #1D4ED8;"
        "}"
        "QPushButton#SendBtn:disabled {"
        "  background-color: #374151;"
        "  color: #6B7280;"
        "}"
        "QPushButton#ScrollDownBtn {"
        "  background-color: rgba(36, 40, 49, 0.9);"
        "  border: 1px solid #363C4A;"
        "  color: #3B82F6;"
        "  border-radius: 12px;"
        "  font-size: 11px;"
        "  padding: 4px 10px;"
        "  font-weight: 500;"
        "}"
        "QPushButton#ScrollDownBtn:hover {"
        "  background-color: #2563EB;"
        "  color: #FFFFFF;"
        "}"
    );

    setObjectName("ChatSidebar");

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(2, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 1. 顶部 Header
    auto *headerWidget = new QWidget(this);
    headerWidget->setFixedHeight(44);
    auto *headerLayout = new QHBoxLayout(headerWidget);
    headerLayout->setContentsMargins(16, 0, 12, 0);

    _titleLabel = new QLabel(QString::fromUtf8("会议聊天"), headerWidget);
    _titleLabel->setObjectName("ChatTitle");
    headerLayout->addWidget(_titleLabel);

    headerLayout->addStretch();

    _closeBtn = new QPushButton("✕", headerWidget);
    _closeBtn->setObjectName("CloseBtn");
    _closeBtn->setCursor(Qt::PointingHandCursor);
    connect(_closeBtn, &QPushButton::clicked, this, &MeetingChatSidebarWidget::closeRequested);
    headerLayout->addWidget(_closeBtn);

    rootLayout->addWidget(headerWidget);

    // 2. 消息滚动展示区
    _scrollArea = new QScrollArea(this);
    _scrollArea->setObjectName("MessageScrollArea");
    _scrollArea->setWidgetResizable(true);
    _scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    _scrollContent = new QWidget();
    _scrollContent->setStyleSheet("background-color: transparent;");
    _messagesLayout = new QVBoxLayout(_scrollContent);
    _messagesLayout->setContentsMargins(0, 8, 0, 8);
    _messagesLayout->setSpacing(4);
    _messagesLayout->addStretch();

    _scrollArea->setWidget(_scrollContent);
    rootLayout->addWidget(_scrollArea, 1);

    connect(_scrollArea->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int value) {
        int max = _scrollArea->verticalScrollBar()->maximum();
        _isAtBottom = (value >= max - 25);
        if (_scrollToBottomBtn) {
            _scrollToBottomBtn->setVisible(!_isAtBottom && !_messages.empty());
        }
    });

    // 3. 底部输入控制区
    _inputContainer = new QWidget(this);
    _inputContainer->setObjectName("BottomInputPanel");
    auto *inputColLayout = new QVBoxLayout(_inputContainer);
    inputColLayout->setContentsMargins(12, 8, 12, 10);
    inputColLayout->setSpacing(6);

    _inputEdit = new ChatInputEdit(_inputContainer);
    connect(_inputEdit, &ChatInputEdit::sendTriggered, this, &MeetingChatSidebarWidget::onSendClicked);
    connect(_inputEdit, &QPlainTextEdit::textChanged, this, &MeetingChatSidebarWidget::onTextChanged);
    connect(_inputEdit, &ChatInputEdit::imagePasted, this, &MeetingChatSidebarWidget::onImagePasted);
    inputColLayout->addWidget(_inputEdit);

    auto *btnRowLayout = new QHBoxLayout();
    btnRowLayout->setContentsMargins(0, 0, 0, 0);
    btnRowLayout->setSpacing(6);

    _imageBtn = new QPushButton(QString::fromUtf8("🖼️ 图片"), _inputContainer);
    _imageBtn->setObjectName("MediaBtn");
    _imageBtn->setToolTip(QString::fromUtf8("发送图片文件"));
    _imageBtn->setCursor(Qt::PointingHandCursor);
    connect(_imageBtn, &QPushButton::clicked, this, &MeetingChatSidebarWidget::onChooseImageClicked);
    btnRowLayout->addWidget(_imageBtn);

    _fileBtn = new QPushButton(QString::fromUtf8("📎 文件"), _inputContainer);
    _fileBtn->setObjectName("MediaBtn");
    _fileBtn->setToolTip(QString::fromUtf8("发送附件文件"));
    _fileBtn->setCursor(Qt::PointingHandCursor);
    connect(_fileBtn, &QPushButton::clicked, this, &MeetingChatSidebarWidget::onChooseFileClicked);
    btnRowLayout->addWidget(_fileBtn);

    _scrollToBottomBtn = new QPushButton(QString::fromUtf8("新消息 ↓"), _inputContainer);
    _scrollToBottomBtn->setObjectName("ScrollDownBtn");
    _scrollToBottomBtn->setCursor(Qt::PointingHandCursor);
    _scrollToBottomBtn->hide();
    connect(_scrollToBottomBtn, &QPushButton::clicked, this, &MeetingChatSidebarWidget::scrollToBottom);
    btnRowLayout->addWidget(_scrollToBottomBtn);

    btnRowLayout->addStretch();

    _sendBtn = new QPushButton(QString::fromUtf8("发送"), _inputContainer);
    _sendBtn->setObjectName("SendBtn");
    _sendBtn->setCursor(Qt::PointingHandCursor);
    _sendBtn->setEnabled(false);
    connect(_sendBtn, &QPushButton::clicked, this, &MeetingChatSidebarWidget::onSendClicked);
    btnRowLayout->addWidget(_sendBtn);

    inputColLayout->addLayout(btnRowLayout);
    rootLayout->addWidget(_inputContainer);

    // 4. 拖拽接收与浮动提示层
    setAcceptDrops(true);
    _scrollArea->setAcceptDrops(true);
    _scrollArea->viewport()->setAcceptDrops(true);
    _scrollContent->setAcceptDrops(true);
    _inputContainer->setAcceptDrops(true);
    _inputEdit->setAcceptDrops(true);

    _scrollArea->installEventFilter(this);
    _scrollArea->viewport()->installEventFilter(this);
    _scrollContent->installEventFilter(this);
    _inputContainer->installEventFilter(this);
    _inputEdit->installEventFilter(this);

    _dropOverlay = new QLabel(this);
    _dropOverlay->setObjectName("DropOverlay");
    _dropOverlay->setAlignment(Qt::AlignCenter);
    _dropOverlay->setText(QString::fromUtf8("📥 松开鼠标以发送文件或图片"));
    _dropOverlay->setStyleSheet(
        "background-color: rgba(30, 58, 138, 0.88);"
        "border: 2px dashed #60A5FA;"
        "border-radius: 8px;"
        "color: #FFFFFF;"
        "font-size: 14px;"
        "font-weight: bold;"
        "font-family: \"Microsoft YaHei\", sans-serif;"
    );
    _dropOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    _dropOverlay->hide();
}

void MeetingChatSidebarWidget::appendMessage(const ChatMessageItem &msg) {
    ChatMessageItem item = msg;
    if (item.seq <= 0) {
        item.seq = (item.timestamp > 0) ? item.timestamp : QDateTime::currentMSecsSinceEpoch();
    }

    if (item.isMine && !item.id.isEmpty()) {
        _pendingMessages[item.id] = item;
    }

    // 智能有序插入：根据 seq 计算插入位置，防止网络波动导致乱序
    int insertIndex = static_cast<int>(_messages.size());
    for (int i = static_cast<int>(_messages.size()) - 1; i >= 0; --i) {
        if (item.seq >= _messages[i].seq) {
            insertIndex = i + 1;
            break;
        }
        insertIndex = i;
    }
    _messages.insert(_messages.begin() + insertIndex, item);

    auto *bubble = new ChatBubbleWidget(item, _scrollContent);
    if (!item.id.isEmpty()) {
        _bubbleMap[item.id] = bubble;
    }
    connect(bubble, &ChatBubbleWidget::retryClicked, this, [this](const QString &id) {
        emit retryRequested(id);
    });

    int count = _messagesLayout->count();
    int targetPos = insertIndex;
    if (count > 0) {
        targetPos = std::min(targetPos, count - 1);
    }
    _messagesLayout->insertWidget(targetPos, bubble);

    _titleLabel->setText(QString::fromUtf8("会议聊天 (%1)").arg(_messages.size()));

    if (_isAtBottom) {
        QTimer::singleShot(10, this, &MeetingChatSidebarWidget::scrollToBottom);
    } else if (_scrollToBottomBtn) {
        _scrollToBottomBtn->show();
    }
}

void MeetingChatSidebarWidget::updateMessageStatus(const QString &messageId, MessageSendStatus status, int progress, const QString &errorMessage) {
    if (_bubbleMap.contains(messageId) && _bubbleMap[messageId]) {
        _bubbleMap[messageId]->updateStatus(status, progress, errorMessage);
    }
    if (_pendingMessages.contains(messageId)) {
        _pendingMessages[messageId].status = status;
        _pendingMessages[messageId].progress = progress;
        _pendingMessages[messageId].errorMessage = errorMessage;
    }
    for (auto &m : _messages) {
        if (m.id == messageId) {
            m.status = status;
            m.progress = progress;
            m.errorMessage = errorMessage;
            break;
        }
    }
}

void MeetingChatSidebarWidget::startReceivingMedia(const QString &transferId, const QString &senderId, const QString &senderName,
                                                 const QString &mediaType, const QString &fileName, qint64 totalSize, qint64 seq) {
    if (_bubbleMap.contains(transferId)) return;

    ChatMessageItem item;
    item.id = transferId;
    item.senderIdentity = senderId;
    item.senderName = senderName;
    item.type = (mediaType == "image") ? ChatMessageType::Image : ChatMessageType::File;
    item.fileName = fileName;
    item.fileSize = totalSize;
    item.timestamp = QDateTime::currentMSecsSinceEpoch();
    item.seq = (seq > 0) ? seq : item.timestamp;
    item.isMine = false;
    item.status = MessageSendStatus::Receiving;
    item.progress = 0;

    appendMessage(item);
}

void MeetingChatSidebarWidget::updateReceivingProgress(const QString &transferId, int progress) {
    if (_bubbleMap.contains(transferId) && _bubbleMap[transferId]) {
        _bubbleMap[transferId]->updateReceivingProgress(progress);
    }
    for (auto &m : _messages) {
        if (m.id == transferId) {
            m.progress = progress;
            break;
        }
    }
}

void MeetingChatSidebarWidget::completeReceivingMedia(const QString &transferId, const QString &/*mediaType*/, const QString &/*fileName*/, const QByteArray &data) {
    if (_bubbleMap.contains(transferId) && _bubbleMap[transferId]) {
        _bubbleMap[transferId]->completeReceivingMedia(data);
    }
    for (auto &m : _messages) {
        if (m.id == transferId) {
            m.fileData = data;
            m.fileSize = data.size();
            m.status = MessageSendStatus::Sent;
            m.progress = 100;
            break;
        }
    }
}

void MeetingChatSidebarWidget::failReceivingMedia(const QString &transferId, const QString &reason) {
    if (_bubbleMap.contains(transferId) && _bubbleMap[transferId]) {
        _bubbleMap[transferId]->failReceivingMedia(reason);
    }
    for (auto &m : _messages) {
        if (m.id == transferId) {
            m.status = MessageSendStatus::Failed;
            m.errorMessage = reason;
            break;
        }
    }
}

void MeetingChatSidebarWidget::clearMessages() {
    _messages.clear();
    _bubbleMap.clear();
    _pendingMessages.clear();
    QLayoutItem *item;
    while ((item = _messagesLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }
    _messagesLayout->addStretch();
    _titleLabel->setText(QString::fromUtf8("会议聊天"));
    if (_scrollToBottomBtn) _scrollToBottomBtn->hide();
}

void MeetingChatSidebarWidget::scrollToBottom() {
    if (!_scrollArea) return;
    auto *vBar = _scrollArea->verticalScrollBar();
    if (vBar) {
        vBar->setValue(vBar->maximum());
    }
    _isAtBottom = true;
    if (_scrollToBottomBtn) {
        _scrollToBottomBtn->hide();
    }
}

void MeetingChatSidebarWidget::onTextChanged() {
    if (!_inputEdit || !_sendBtn) return;
    QString text = _inputEdit->toPlainText().trimmed();
    _sendBtn->setEnabled(!text.isEmpty());
}

void MeetingChatSidebarWidget::onSendClicked() {
    if (!_inputEdit) return;
    QString text = _inputEdit->toPlainText().trimmed();
    if (text.isEmpty()) return;

    _inputEdit->clear();
    emit messageSent(text);
}

void MeetingChatSidebarWidget::onChooseImageClicked() {
    QString path = QFileDialog::getOpenFileName(this, QString::fromUtf8("选择发送图片"), "",
                                                "图片文件 (*.png *.jpg *.jpeg *.bmp *.gif);;所有文件 (*.*)");
    if (path.isEmpty()) return;

    QFileInfo fi(path);
    if (fi.size() > 15 * 1024 * 1024) {
        QMessageBox::warning(this, QString::fromUtf8("禁止发送"),
                             QString::fromUtf8("图片 \"%1\" 大小为 %2，超过 15MB 限制，禁止发送。")
                             .arg(fi.fileName())
                             .arg(ChatBubbleWidget::formatFileSize(fi.size())));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QString::fromUtf8("打开失败"), QString::fromUtf8("无法读取选中的图片文件。"));
        return;
    }
    QByteArray data = file.readAll();
    file.close();

    QString fileName = fi.fileName();
    emit imageSent(fileName, data);
}

void MeetingChatSidebarWidget::onChooseFileClicked() {
    QString path = QFileDialog::getOpenFileName(this, QString::fromUtf8("选择发送文件"), "", "所有文件 (*.*)");
    if (path.isEmpty()) return;

    QFileInfo fi(path);
    if (fi.size() > 15 * 1024 * 1024) {
        QMessageBox::warning(this, QString::fromUtf8("禁止发送"),
                             QString::fromUtf8("文件 \"%1\" 大小为 %2，超过 15MB 限制，禁止发送。")
                             .arg(fi.fileName())
                             .arg(ChatBubbleWidget::formatFileSize(fi.size())));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QString::fromUtf8("打开失败"), QString::fromUtf8("无法读取选中的文件。"));
        return;
    }
    QByteArray data = file.readAll();
    file.close();

    QString fileName = fi.fileName();
    emit fileSent(fileName, data);
}

void MeetingChatSidebarWidget::onImagePasted(const QImage &image) {
    if (image.isNull()) return;

    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");

    QString fileName = QString("screenshot_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    emit imageSent(fileName, data);
}

void MeetingChatSidebarWidget::paintEvent(QPaintEvent *e) {
    Q_UNUSED(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    p.fillRect(rect(), QColor("#1A1D24"));

    p.setPen(QPen(QColor(8, 10, 14), 1));
    p.drawLine(0, 0, 0, height());

    p.setPen(QPen(QColor(58, 66, 82), 1));
    p.drawLine(1, 0, 1, height());

    QLinearGradient shadow(2, 0, 10, 0);
    shadow.setColorAt(0.0, QColor(0, 0, 0, 45));
    shadow.setColorAt(1.0, QColor(0, 0, 0, 0));
    p.fillRect(QRect(2, 0, 8, height()), shadow);
}

void MeetingChatSidebarWidget::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    if (_dropOverlay && _dropOverlay->isVisible()) {
        _dropOverlay->setGeometry(rect().adjusted(10, 48, -10, -10));
    }
}

bool MeetingChatSidebarWidget::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::DragEnter) {
        auto *de = static_cast<QDragEnterEvent *>(event);
        if (de->mimeData() && (de->mimeData()->hasUrls() || de->mimeData()->hasImage())) {
            de->acceptProposedAction();
            _isDraggingOver = true;
            if (_dropOverlay) {
                _dropOverlay->setGeometry(rect().adjusted(10, 48, -10, -10));
                _dropOverlay->raise();
                _dropOverlay->show();
            }
            return true;
        }
    } else if (event->type() == QEvent::DragMove) {
        auto *dm = static_cast<QDragMoveEvent *>(event);
        if (dm->mimeData() && (dm->mimeData()->hasUrls() || dm->mimeData()->hasImage())) {
            dm->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::DragLeave) {
        QPoint localPos = mapFromGlobal(QCursor::pos());
        if (!rect().contains(localPos)) {
            _isDraggingOver = false;
            if (_dropOverlay) {
                _dropOverlay->hide();
            }
        }
        return true;
    } else if (event->type() == QEvent::Drop) {
        auto *dp = static_cast<QDropEvent *>(event);
        _isDraggingOver = false;
        if (_dropOverlay) {
            _dropOverlay->hide();
        }
        if (dp->mimeData()) {
            dp->acceptProposedAction();
            handleDropMimeData(dp->mimeData());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MeetingChatSidebarWidget::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData() && (e->mimeData()->hasUrls() || e->mimeData()->hasImage())) {
        e->acceptProposedAction();
        _isDraggingOver = true;
        if (_dropOverlay) {
            _dropOverlay->setGeometry(rect().adjusted(10, 48, -10, -10));
            _dropOverlay->raise();
            _dropOverlay->show();
        }
    }
}

void MeetingChatSidebarWidget::dragMoveEvent(QDragMoveEvent *e) {
    if (e->mimeData() && (e->mimeData()->hasUrls() || e->mimeData()->hasImage())) {
        e->acceptProposedAction();
    }
}

void MeetingChatSidebarWidget::dragLeaveEvent(QDragLeaveEvent *e) {
    Q_UNUSED(e);
    QPoint localPos = mapFromGlobal(QCursor::pos());
    if (!rect().contains(localPos)) {
        _isDraggingOver = false;
        if (_dropOverlay) {
            _dropOverlay->hide();
        }
    }
}

void MeetingChatSidebarWidget::dropEvent(QDropEvent *e) {
    _isDraggingOver = false;
    if (_dropOverlay) {
        _dropOverlay->hide();
    }
    if (e->mimeData()) {
        e->acceptProposedAction();
        handleDropMimeData(e->mimeData());
    }
}

void MeetingChatSidebarWidget::handleDropMimeData(const QMimeData *mimeData) {
    if (!mimeData) return;

    QStringList filePaths;
    if (mimeData->hasUrls()) {
        for (const QUrl &url : mimeData->urls()) {
            if (url.isLocalFile()) {
                QString path = url.toLocalFile();
                QFileInfo fi(path);
                if (fi.exists() && fi.isFile()) {
                    filePaths.append(path);
                }
            }
        }
    }

    if (filePaths.isEmpty() && mimeData->hasImage()) {
        QImage img = qvariant_cast<QImage>(mimeData->imageData());
        if (!img.isNull()) {
            onImagePasted(img);
            return;
        }
    }

    if (filePaths.isEmpty()) return;

    handleDroppedFiles(filePaths);
}

void MeetingChatSidebarWidget::handleDroppedFiles(const QStringList &filePaths) {
    if (filePaths.isEmpty()) return;

    if (filePaths.size() == 1) {
        QFileInfo fi(filePaths.first());
        if (fi.size() > 15 * 1024 * 1024) {
            QMessageBox::warning(this, QString::fromUtf8("禁止发送"),
                                 QString::fromUtf8("文件 \"%1\" 大小为 %2，超过 15MB 限制，禁止发送。")
                                 .arg(fi.fileName())
                                 .arg(ChatBubbleWidget::formatFileSize(fi.size())));
            return;
        }
    } else {
        bool allOversize = true;
        for (const QString &path : filePaths) {
            if (QFileInfo(path).size() <= 15 * 1024 * 1024) {
                allOversize = false;
                break;
            }
        }
        if (allOversize) {
            QMessageBox::warning(this, QString::fromUtf8("禁止发送"),
                                 QString::fromUtf8("所选文件均超过 15MB 限制，禁止发送。"));
            return;
        }
    }

    SendConfirmDialog dlg(filePaths, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    static const QStringList imgExts = {"png", "jpg", "jpeg", "bmp", "gif", "webp"};

    for (const QString &path : filePaths) {
        QFileInfo fi(path);
        if (!fi.exists() || !fi.isFile()) continue;
        if (fi.size() > 15 * 1024 * 1024) continue;

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QByteArray data = file.readAll();
        file.close();

        QString ext = fi.suffix().toLower();
        if (imgExts.contains(ext)) {
            emit imageSent(fi.fileName(), data);
        } else {
            emit fileSent(fi.fileName(), data);
        }
    }
}

} // namespace OpenMeeting
