#pragma once
#include "meeting_encryption_dialog.h"
#include "src/e2ee/media_encryption_status.h"
#include <QtCore/QPointer>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QTableWidget>
#include <functional>

namespace MeetingUI {

// Policy and recovery UI. Installation alone never renders a verified/green lock.
class MeetingEncryptionPanel final : public QWidget {
public:
    explicit MeetingEncryptionPanel(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("meetingEncryptionPanel"));
        AppTheme::applySurfacePalette(*this, AppTheme::Tone::Dark);
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(12, 4, 12, 4);
        label_ = new QLabel(this);
        label_->setObjectName(QStringLiteral("encryptionPolicyStatus"));
        label_->setWordWrap(true);
        row->addWidget(label_, 1);
        media_ = new QLabel(this);
        media_->setObjectName(QStringLiteral("encryptionMediaStatus"));
        media_->setWordWrap(true);
        row->addWidget(media_, 1);
        details_ = new QPushButton(trText("Protection details"), this);
        details_->setObjectName(QStringLiteral("encryptionProtectionDetails"));
        row->addWidget(details_);
        connect(details_, &QPushButton::clicked, this, [this] {
            if (!required_ || !connected_) return;
            if (detailsDialog_) { detailsDialog_->raise(); detailsDialog_->activateWindow(); return; }
            auto* dialog = new QDialog(this);
            detailsDialog_ = dialog;
            dialog->setObjectName(QStringLiteral("encryptionProtectionDetailsDialog"));
            dialog->setWindowTitle(trText("Protection details"));
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            connect(dialog, &QDialog::finished, this, [this, dialog](int) {
                if (detailsDialog_ == dialog) { detailsDialog_.clear(); table_.clear(); }
            });
            AppTheme::configureModelessWindow(*dialog);
            AppTheme::applySurfacePalette(*dialog, AppTheme::Tone::Dark);
            AppTheme::setStyleVariant(*dialog, "telemetry-panel");
            auto* layout = new QVBoxLayout(dialog);
            auto* scope = new QLabel(trText("Only this client's active media is shown. This does not verify every participant or the whole room."), dialog);
            scope->setWordWrap(true);
            layout->addWidget(scope);
            table_ = new QTableWidget(dialog);
            table_->setObjectName(QStringLiteral("encryptionProtectionTracks"));
            table_->setColumnCount(5);
            table_->setHorizontalHeaderLabels({trText("Participant"), trText("Track"), trText("Direction"), trText("Media"), trText("Protection")});
            table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
            table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
            table_->horizontalHeader()->setStretchLastSection(true);
            table_->verticalHeader()->hide();
            layout->addWidget(table_);
            populateDetails();
            AppTheme::makeDialogAdaptive(*dialog, QSize(820, 360));
            dialog->show();
        });
        recover_ = new QPushButton(trText("Re-enter encryption key"), this);
        recover_->setObjectName(QStringLiteral("reenterEncryptionKey"));
        recover_->setProperty("meetingUiRole", "secondary");
        row->addWidget(recover_);
        connect(recover_, &QPushButton::clicked, this, [this] {
            if (!canRecover_ || dialog_) return;
            dialog_ = new MeetingEncryptionDialog(this, true);
            dialog_->setAttribute(Qt::WA_DeleteOnClose);
            recover_->setEnabled(false);
            const auto dialog = dialog_;
            connect(dialog, &QDialog::finished, this, [this, dialog](int result) {
                dialog_.clear();
                auto request = dialog ? dialog->takeRequest() : std::nullopt;
                if (result != QDialog::Accepted || !request) {
                    recover_->setEnabled(canRecover_);
                    return;
                }
                if (!canRecover_ || !handler_) {
                    request->Revoke();
                    recover_->setEnabled(canRecover_);
                    return;
                }
                canRecover_ = false;
                clearMediaStatus();
                label_->setText(trText("Installing encryption key..."));
                // The callback may synchronously destroy this panel.
                const auto handler = handler_;
                handler(std::move(request->secret));
            });
            dialog_->open();
        });
        setSession(false, false, false);
    }
    void setRecoveryHandler(std::function<void(std::shared_ptr<livekit::MeetingSecretHandle>)> handler) {
        handler_ = std::move(handler);
    }
    void setSession(bool required, bool connected, bool canRecover) {
        const bool changed = !initialized_ || required_ != required || connected_ != connected;
        initialized_ = true;
        required_ = required;
        connected_ = connected;
        if (changed) clearMediaStatus();
        canRecover_ = required && connected && canRecover;
        if ((!required || !connected) && dialog_) dialog_->reject();
        recover_->setVisible(required);
        details_->setVisible(required);
        details_->setEnabled(required && connected);
        recover_->setEnabled(canRecover_ && !dialog_);
        if (changed) label_->setText(!required ? trText("End-to-end encryption is off.") :
            connected ? trText("End-to-end encryption required; media verification pending.") :
                        trText("Waiting for connection; encryption verification pending."));
    }
    void setInstallResult(bool installed, bool canRecover) {
        canRecover_ = canRecover;
        recover_->setEnabled(canRecover_ && !dialog_);
        label_->setText(installed ? trText("Key installed; media verification pending.") :
            trText("Key could not be installed. Re-enter the shared key."));
    }
    void setMediaStatus(const livekit::MediaEncryptionStatus& status) {
        if (!required_ || !connected_) return;
        mediaStatus_ = status;
        populateDetails();
        if (!status.enabled) {
            label_->setText(trText("End-to-end encryption required; media verification pending."));
            media_->setText(trText("Media encryption is unavailable."));
            return;
        }
        unsigned sending = 0, receiving = 0, waiting = 0, failed = 0;
        unsigned protectedSending = 0, protectedReceiving = 0;
        for (const auto& track : status.tracks) {
            track.receiving ? ++receiving : ++sending;
            const bool error = track.report == livekit::MediaCryptorReport::MissingKey ||
                track.report == livekit::MediaCryptorReport::Failed;
            if (error) ++failed;
            if (!error && track.protected_after_current_install)
                track.receiving ? ++protectedReceiving : ++protectedSending;
            else ++waiting;
        }
        label_->setText(!status.tracks.empty() && !waiting && !failed ?
            trText("End-to-end protection observed on this client's active media.") :
            trText("End-to-end encryption required; media verification pending."));
        media_->setText(trText("Media protection: sending %1/%2, receiving %3/%4; pending %5; errors %6.")
            .arg(protectedSending).arg(sending).arg(protectedReceiving).arg(receiving).arg(waiting).arg(failed));
    }
    void clearMediaStatus() {
        mediaStatus_ = {};
        media_->clear();
        populateDetails();
    }
private:
    void populateDetails() {
        if (!table_) return;
        table_->setRowCount(static_cast<int>(mediaStatus_.tracks.size()));
        for (int row = 0; row < table_->rowCount(); ++row) {
            const auto& track = mediaStatus_.tracks[row];
            const char* protection = track.report == livekit::MediaCryptorReport::MissingKey ? "Missing key" :
                track.report == livekit::MediaCryptorReport::Failed ? "Protection failed" :
                track.protected_after_current_install ? "Protected frame observed" : "Verification pending";
            const QStringList values{QString::fromStdString(track.participant_identity), QString::fromStdString(track.track_id),
                trText(track.receiving ? "Receiving" : "Sending"), trText(track.video ? "Video" : "Audio"), trText(protection)};
            for (int col = 0; col < values.size(); ++col) table_->setItem(row, col, new QTableWidgetItem(values[col]));
        }
    }
    static QString trText(const char* text) { return QCoreApplication::translate("MeetingUI", text); }
    QLabel* label_ = nullptr;
    QLabel* media_ = nullptr;
    QPushButton* recover_ = nullptr;
    QPushButton* details_ = nullptr;
    QPointer<QDialog> detailsDialog_;
    QPointer<QTableWidget> table_;
    livekit::MediaEncryptionStatus mediaStatus_;
    QPointer<MeetingEncryptionDialog> dialog_;
    bool canRecover_ = false;
    bool initialized_ = false;
    bool required_ = false;
    bool connected_ = false;
    std::function<void(std::shared_ptr<livekit::MeetingSecretHandle>)> handler_;
};
}
