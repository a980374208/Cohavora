#pragma once

#include "src/e2ee/meeting_encryption.h"
#include "src/ui/app_theme.h"
#include <QtCore/QCoreApplication>
#include <QtCore/qscopeguard.h>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <optional>

namespace MeetingUI {

// Shared by quick, ordinary, direct, detail and automatic-share admission.
// The dialog owns plaintext only while editing; the caller receives a revocable handle.
class MeetingEncryptionDialog final : public QDialog {
public:
    explicit MeetingEncryptionDialog(QWidget* parent = nullptr, bool recovery = false) : QDialog(parent), recovery_(recovery) {
        const auto tr = [](const char* value) { return QCoreApplication::translate("MeetingUI", value); };
        setObjectName(QStringLiteral("meetingEncryptionDialog"));
        setWindowTitle(tr("Meeting Encryption"));
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        AppTheme::setStyleVariant(*this, "meeting-main-window-this");
        resize(480, 360);
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(12, 12, 12, 12);
        auto* surface = new QWidget(this);
        surface->setObjectName(QStringLiteral("dialogContainer"));
        AppTheme::applySurfacePalette(*surface, AppTheme::Tone::Light);
        outer->addWidget(surface);
        auto* layout = new QVBoxLayout(surface);
        layout->setContentsMargins(28, 24, 28, 24);
        layout->setSpacing(12);
        auto* title = new QLabel(tr("Meeting Encryption"), surface);
        auto font = title->font(); font.setBold(true); font.setPixelSize(18); title->setFont(font);
        layout->addWidget(title);
        required_ = new QCheckBox(tr("Require end-to-end encryption for this session"), surface);
        required_->setObjectName(QStringLiteral("e2eeRequired"));
        layout->addWidget(required_);
        auto* info = new QLabel(tr("Use the same encryption key as the other participants. Share it through a trusted channel. The key is not saved. Participant and connection metadata remain visible to the service."), surface);
        info->setWordWrap(true);
        layout->addWidget(info);
        auto* profile = new QLabel(tr("Encrypted meetings support VP8 and H264. Auto uses VP8; select a supported codec in Settings for camera and screen sharing."), surface);
        profile->setWordWrap(true);
        layout->addWidget(profile);
        key_ = new QLineEdit(surface);
        key_->setObjectName(QStringLiteral("e2eeKeyInput"));
        key_->setAccessibleName(tr("Encryption key"));
        key_->setPlaceholderText(tr("ASCII encryption key (not the meeting password)"));
        key_->setEchoMode(QLineEdit::Password);
        key_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
        key_->setContextMenuPolicy(Qt::NoContextMenu);
        // Keep an overflow sentinel so overlong paste is rejected, not silently
        // truncated into a different accepted key. Storage remains bounded.
        key_->setMaxLength(4097);
        auto inputPalette = surface->palette();
        auto placeholder = inputPalette.color(QPalette::Text);
        placeholder.setAlphaF(0.55);
        inputPalette.setColor(QPalette::PlaceholderText, placeholder);
        key_->setPalette(inputPalette);
        key_->setEnabled(false);
        layout->addWidget(key_);
        error_ = new QLabel(surface);
        error_->setObjectName(QStringLiteral("e2eeInputError"));
        AppTheme::setStyleVariant(*error_, "meeting-main-window-statuslabel");
        error_->setWordWrap(true);
        layout->addWidget(error_);
        auto* buttons = new QHBoxLayout;
        auto* cancel = new QPushButton(tr("Cancel"), surface);
        auto* proceed = new QPushButton(tr("Continue"), surface);
        cancel->setObjectName(QStringLiteral("cancelBtn"));
        proceed->setObjectName(QStringLiteral("joinBtn"));
        // Enter from the editor must validate, not select the first (Cancel) button.
        proceed->setDefault(true);
        buttons->addWidget(cancel); buttons->addWidget(proceed);
        layout->addLayout(buttons);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(proceed, &QPushButton::clicked, this, &QDialog::accept);
        connect(required_, &QCheckBox::toggled, this, [this](bool required) {
            // Latch before enabling plaintext entry, including cancelled edits.
            if (required) livekit::MarkSensitiveMemoryUsed();
            key_->setEnabled(required);
            if (!required) clearEditor();
            error_->clear();
        });
        if (recovery_) {
            required_->setChecked(true);
            required_->setEnabled(false);
        }
    }
    ~MeetingEncryptionDialog() override {
        clearEditor();
        if (request_) request_->Revoke();
    }
    std::optional<livekit::MeetingEncryptionRequest> takeRequest() {
        return std::exchange(request_, std::nullopt);
    }
    void reject() override {
        clearEditor();
        if (request_) request_->Revoke();
        request_.reset();
        QDialog::reject();
    }
    void accept() override {
        livekit::MeetingEncryptionRequest request;
        if (recovery_ || required_->isChecked()) {
            auto text = key_->text();
            auto bytes = text.toUtf8();
            text.fill(QChar(0)); text.clear();
            const auto cleanup = qScopeGuard([&] {
                if (!bytes.isEmpty()) OPENSSL_cleanse(bytes.data(), static_cast<size_t>(bytes.size()));
            });
            try {
                request.mode = livekit::MeetingEncryptionMode::Required;
                request.secret = livekit::MeetingSecretHandle::Create(
                    std::vector<uint8_t>(bytes.begin(), bytes.end()));
            } catch (const livekit::EncryptionRequestException&) {
                error_->setText(QCoreApplication::translate("MeetingUI", "Enter 1 to 4096 printable ASCII characters. Unicode keys are not supported."));
                return;
            }
        }
        clearEditor();
        if (request_) request_->Revoke();
        request_ = std::move(request);
        QDialog::accept();
    }
private:
    void clearEditor() {
        if (!key_) return;
        // setText resets the undo history. Qt/IME historical copies cannot be guaranteed erased.
        key_->setText(QString(key_->text().size(), QChar(0)));
        key_->clear();
    }
    bool recovery_ = false;
    QCheckBox* required_ = nullptr;
    QLineEdit* key_ = nullptr;
    QLabel* error_ = nullptr;
    std::optional<livekit::MeetingEncryptionRequest> request_;
};

inline std::optional<livekit::MeetingEncryptionRequest> PromptMeetingEncryption(QWidget* parent) {
    MeetingEncryptionDialog dialog(parent);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return dialog.takeRequest();
}

} // namespace MeetingUI
