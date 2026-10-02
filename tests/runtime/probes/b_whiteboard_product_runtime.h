#pragma once

#include "src/core/meeting_coordinator.h"
#include "src/ui/whiteboard/whiteboard_renderer.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtGui/QImage>

// Drives public product commands and observes immutable Qt projections only.
// The renderer/QSaveFile path matches production export; dialog UX is covered
// separately by the original B01/B02 UIA gates.
class BWhiteboardProductObserver final {
public:
    using Emit = std::function<void(const char *, QJsonObject)>;
    BWhiteboardProductObserver(OpenMeeting::MeetingCoordinator &owner, QString root, Emit record)
        : owner_(owner), root_(std::move(root)), emit_(std::move(record)) {
        QObject::connect(&owner_, &OpenMeeting::MeetingCoordinator::whiteboardProjectionChanged,
            &owner_, [this](const QByteArray &snapshot, quint64 sequence, int state,
                const QString &authority, const QString &actor, bool locked, bool writers,
                bool edit, bool admin, const QVariantMap &assets, const QString &) {
                snapshot_ = snapshot;
                for (auto it = assets.begin(); it != assets.end(); ++it) assets_.insert(it.key(), it.value());
                state_ = QJsonObject{{"sequence", static_cast<qint64>(sequence)}, {"state", state},
                    {"authority", authority}, {"actor", actor}, {"locked", locked},
                    {"writers_open", writers}, {"can_edit", edit}, {"can_admin", admin}};
                observe();
            });
    }
    bool handle(const QJsonObject &command) {
        const auto action = command.value("action").toString();
        if (!action.startsWith("board_")) return false;
        const int sequence = command.value("sequence").toInt();
        if (action == "board_observe") {
            observe(sequence);
            return true;
        }
        if (action == "board_reconnect") {
            if (auto room = owner_.room()) room->SimulateScenario(livekit::SimulateScenarioType::SignalReconnect);
            else emit_("board_command_failed", {{"sequence", sequence}});
            return true;
        }
        const auto doc = livekit::whiteboard::Document::fromJson(
            std::string_view(snapshot_.constData(), static_cast<std::size_t>(snapshot_.size())));
        if (!doc) {
            emit_("board_command_failed", {{"sequence", sequence}});
            return true;
        }
        if (action == "board_add") {
            livekit::whiteboard::Command proposal;
            proposal.id = command.value("id").toString().toStdString();
            proposal.context = doc->context();
            proposal.kind = livekit::whiteboard::CommandKind::Add;
            proposal.object.id = "object-" + proposal.id;
            proposal.object.kind = livekit::whiteboard::ObjectKind::Rectangle;
            proposal.object.color = static_cast<std::uint32_t>(command.value("color").toInt(0x1677ff));
            proposal.object.width = 4;
            const double x = command.value("x").toDouble(10);
            proposal.object.points = {{x, 10}, {x + 30, 40}};
            owner_.submitWhiteboardCommand(proposal);
            emit_("board_proposed", {{"sequence", sequence}, {"id", QString::fromStdString(proposal.id)}});
        } else if (action == "board_image") {
            const auto path = QFileInfo(command.value("path").toString()).canonicalFilePath();
            const auto payloadRoot = QFileInfo(root_ + "/../payloads").canonicalFilePath();
            QFile input(path);
            if (payloadRoot.isEmpty() || !path.startsWith(payloadRoot + "/") || !input.open(QIODevice::ReadOnly)) {
                emit_("board_command_failed", {{"sequence", sequence}});
                return true;
            }
            const auto bytes = input.readAll();
            const auto image = QImage::fromData(bytes, "PNG");
            if (image.isNull()) { emit_("board_command_failed", {{"sequence", sequence}}); return true; }
            const auto id = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
            owner_.submitWhiteboardImage(bytes, id, image.width(), image.height(),
                command.value("page").toString(), false);
            emit_("board_image_proposed", {{"sequence", sequence}, {"asset", id}});
        } else if (action == "board_export") {
            QImage background;
            if (!doc->page().backgroundAssetId.empty()) {
                background = QImage::fromData(assets_.value(QString::fromStdString(doc->page().backgroundAssetId)).toByteArray(), "PNG");
                if (background.isNull()) { emit_("board_command_failed", {{"sequence", sequence}}); return true; }
            }
            const auto image = MeetingUI::WhiteboardRendering::renderPage(doc->page(), background);
            const auto path = root_ + "/export-" + QString::number(sequence) + ".png";
            QSaveFile file(path);
            const bool saved = !image.isNull() && file.open(QIODevice::WriteOnly) && image.save(&file, "PNG") && file.commit();
            emit_("board_exported", {{"sequence", sequence}, {"saved", saved}, {"path", path}});
        } else {
            emit_("board_command_failed", {{"sequence", sequence}});
        }
        return true;
    }
private:
    void observe(int commandSequence = 0) {
        auto state = state_;
        state.insert("command_sequence", commandSequence);
        state.insert("document", QJsonDocument::fromJson(snapshot_).object());
        state.insert("document_sha256", QString::fromLatin1(
            QCryptographicHash::hash(snapshot_, QCryptographicHash::Sha256).toHex()));
        QJsonObject assetHashes;
        for (auto it = assets_.begin(); it != assets_.end(); ++it) {
            assetHashes.insert(it.key(), QString::fromLatin1(QCryptographicHash::hash(
                it.value().toByteArray(), QCryptographicHash::Sha256).toHex()));
        }
        state.insert("asset_hashes", assetHashes);
        QSaveFile file(root_ + "/board.json");
        const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            emit_("board_command_failed", {{"sequence", commandSequence}});
            return;
        }
        emit_("board_projection", state);
    }
    OpenMeeting::MeetingCoordinator &owner_;
    QString root_;
    Emit emit_;
    QByteArray snapshot_;
    QJsonObject state_;
    QVariantMap assets_;
};
