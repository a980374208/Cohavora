#pragma once
#include "src/ui/meeting_room_window.h"
#include <QtGui/QClipboard>
#include <QtWidgets/QApplication>
#include <QtCore/QMimeData>
#include <QtCore/QSet>
#include <QtCore/QJsonObject>

// Existing narrow test friend; only the invitation prerequisites and notice
// effect are controlled. The production formatter and clipboard write run.
class ParticipantWindowTestAccess final {
public:
    static bool invitation(MeetingUI::MeetingRoomWindow& window, bool business) {
        const auto mode = window._config.invitationMode;
        auto oldNotice = std::move(window._invitationNoticeEffect);
        window._config.invitationMode = business ? MeetingUI::InvitationMode::BusinessMeetingId : MeetingUI::InvitationMode::Disabled;
        bool called = false, copied = false;
        window._invitationNoticeEffect = [&](bool success, const QString&, const QString&) { called = true; copied = success; };
        window.handleInviteClicked();
        window._invitationNoticeEffect = std::move(oldNotice);
        window._config.invitationMode = mode;
        return called && copied == business;
    }
};
class E2eeInvitationEvidence final {
public:
    E2eeInvitationEvidence() {
        if (qEnvironmentVariable("E2EE_OUTPUT_EVIDENCE") != "1") return;
        for (const auto* name : {"E2EE_TEST_KEY", "E2EE_NEXT_TEST_KEY", "LIVEKIT_TOKEN", "LIVEKIT_URL"}) {
            auto value = qEnvironmentVariable(name); if (!value.isEmpty()) secrets_.push_back(value);
        }
    }
    bool needs(quint64 epoch) const { return !secrets_.empty() && (epoch == 4 || epoch == 6) && !seen_.contains(epoch); }
    QJsonObject inspect(MeetingUI::MeetingRoomWindow& window, const QString& meetingId, quint64 epoch) {
        seen_.insert(epoch);
        auto* clipboard = QApplication::clipboard();
        auto restore = std::make_unique<QMimeData>();
        if (const auto* old = clipboard->mimeData()) for (const auto& format : old->formats()) restore->setData(format, old->data(format));
        const auto priorText = clipboard->text();
        const bool disabled = ParticipantWindowTestAccess::invitation(window, false) && clipboard->text() == priorText;
        const bool copied = ParticipantWindowTestAccess::invitation(window, true);
        const auto text = clipboard->text();
        const auto expected = QCoreApplication::translate("MeetingUI", "[Cohavora Meeting Invitation]\nMeeting ID: %1\nSign in with your own account in a client configured for the same meeting service, then join using this meeting ID.").arg(meetingId);
        bool secretFree = true; for (const auto& secret : secrets_) secretFree = secretFree && !text.contains(secret);
        clipboard->setMimeData(restore.release());
        return {{"epoch", static_cast<qint64>(epoch)}, {"valid", disabled && copied && text == expected && secretFree},
            {"disabled_unchanged", disabled}, {"notice_success", copied}, {"actual_length", text.size()}, {"expected_length", expected.size()}, {"contains_meeting_id", !meetingId.isEmpty() && text.contains(meetingId)}, {"business_id_only", copied && text == expected}, {"secret_free", secretFree}};
    }
private:
    std::vector<QString> secrets_;
    QSet<quint64> seen_;
};
