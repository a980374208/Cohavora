#pragma once

#include "src/core/meeting_coordinator.h"
#include "src/core/session_shutdown_service.h"
#include "src/net/session_manager.h"
#include "src/net/service_endpoint_policy.h"
#include "tests/runtime/probes/b_whiteboard_product_runtime.h"
#include "tests/runtime/probes/b_network_product_runtime.h"
#include "tests/runtime/probes/b_telemetry_product_runtime.h"
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <future>

namespace OpenMeeting {
// Read-only fixture observation: UI queues on their Qt owner, native assembly
// maps on the session strand. No state or callbacks are injected.
class MeetingCoordinatorTestAccess final {
public:
    static bool fileSessionReady(const MeetingCoordinator &owner) {
        return owner._state == MeetingState::InMeeting && owner._startupCommitted &&
            !owner._startupListenOnly && !owner._startupReconnectPending;
    }
    static std::shared_ptr<MeetingSessionRuntime> telemetrySession(const MeetingCoordinator &owner) {
        return owner._sessionRuntime;
    }
    static std::future<QJsonObject> fileState(MeetingCoordinator &owner) {
        auto promise = std::make_shared<std::promise<QJsonObject>>();
        auto future = promise->get_future();
        int pending = 0;
        for (const auto &[id, entry] : owner._inboundTransferLedger) if (!entry.second) ++pending;
        QJsonObject ui{{"send_queue", static_cast<int>(owner._mediaSendQueue.size())},
                      {"inbound_pending", pending}};
        auto session = owner._sessionRuntime;
        if (!session) {
            ui.insert("assemblies", 0);
            promise->set_value(std::move(ui));
        } else if (!session->post([session, promise, ui = std::move(ui)]() mutable {
            ui.insert("assemblies", static_cast<int>(session->transfersOnStrand().size()));
            promise->set_value(std::move(ui));
        })) {
            promise->set_exception(std::make_exception_ptr(std::runtime_error("snapshot_not_admitted")));
        }
        return future;
    }
};
} // namespace OpenMeeting

inline int RunBAcceptanceFile(QApplication &app) {
    using namespace OpenMeeting;
    const auto root = QFileInfo(qEnvironmentVariable("B_FILE_ROOT")).canonicalFilePath();
    const auto identity = qEnvironmentVariable("B_FILE_IDENTITY");
    const auto token = qEnvironmentVariable("LIVEKIT_TOKEN");
    const auto url = qEnvironmentVariable("LIVEKIT_URL");
    const auto roomName = qEnvironmentVariable("B_FILE_ROOM");
    if (root.isEmpty() || identity.isEmpty() || token.isEmpty()) return 2;
    QDir(root).mkpath("received");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root + "/settings");
    initializeServiceEndpointPolicy(true);
    SessionManager::instance().loginAsGuest("B File Fixture", identity);
    QFile output(root + "/events.jsonl");
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
    const auto emitEvent = [&](const char *event, QJsonObject data = {}) {
        data.insert("event", QString::fromLatin1(event));
        data.insert("utc_ms", QDateTime::currentMSecsSinceEpoch());
        output.write(QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n');
        output.flush();
    };
    const int telemetryMode=qEnvironmentVariableIntValue("B_TELEMETRY_MODE");
    std::unique_ptr<BTelemetryProductObserver> performance;
    if (telemetryMode==1 || telemetryMode==2) {
        try { performance=std::make_unique<BTelemetryProductObserver>(root,telemetryMode==2,emitEvent); }
        catch (const std::exception &error) {
            emitEvent("telemetry_fixture_start_failed",{{"reason",QString::fromUtf8(error.what())}});
            return 2;
        }
    }
    auto coordinator = MeetingCoordinator::create();
    std::unique_ptr<BWhiteboardProductObserver> board;
    if (qEnvironmentVariableIntValue("B_BOARD_MODE") == 1)
        board = std::make_unique<BWhiteboardProductObserver>(*coordinator, root, emitEvent);
    MediaPreferences preferences;
    preferences.enableMicrophone = false;
    preferences.enableVideo = qEnvironmentVariableIntValue("B_GENERATED_VIDEO") == 1;
    SessionManager::instance().setMediaPreferences(preferences);
    std::unique_ptr<BNetworkProductObserver> network;
    if (qEnvironmentVariableIntValue("B_NETWORK_MODE") == 1) {
        app.setQuitOnLastWindowClosed(false);
        network = std::make_unique<BNetworkProductObserver>(coordinator, preferences.enableVideo,
            qEnvironmentVariableIntValue("B_NETWORK_SHOW_UI") == 1, emitEvent);
    }
    std::vector<std::weak_ptr<livekit::Room>> rooms;
    std::future<QJsonObject> stateFuture;
    std::map<QString, QString> receivingNames;
    bool failed = false;
    bool rejectedJoinPending = false;
    int joins = 0, snapshotSequence = 0;
    bool joinPending = false;
    const auto join = [&] {
        joinPending = true;
        const auto admission = [&] {
            coordinator->connectDirectlyAsync(url, token, roomName, "B File Fixture", preferences);
            if (performance) performance->afterAdmission(coordinator->room());
        };
        if (network) network->prepareJoin(admission);
        else admission();
    };
    QObject::connect(coordinator.get(), &MeetingCoordinator::stateChanged, &app,
        [&](MeetingState state, const QString &) {
            emitEvent("state", {{"state", static_cast<int>(state)}});
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::meetingLeft, &app,
        [&] { emitEvent("left"); });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMessageSendProgress, &app,
        [&](const QString &id, int percent) { emitEvent("send_progress", {{"message",id},{"percent",percent}}); });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMessageSendSuccess, &app,
        [&](const QString &id) { emitEvent("send_success", {{"message",id}}); });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMessageSendFailed, &app,
        [&](const QString &id, const QString &) { emitEvent("send_failed", {{"message",id}}); });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMediaReceivingStarted, &app,
        [&](const QString &id, const QString &, const QString &, const QString &type,
            const QString &name, qint64 size, qint64) {
            receivingNames[id] = name;
            emitEvent("receive_started", {{"transfer",id},{"name",name},{"type",type},{"bytes",size}});
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMediaReceivingProgress, &app,
        [&](const QString &id, int percent) {
            emitEvent("receive_progress", {{"transfer",id},{"name",receivingNames[id]},{"percent",percent}});
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMediaReceivingFailed, &app,
        [&](const QString &id, const QString &) {
            emitEvent("receive_failed", {{"transfer",id},{"name",receivingNames[id]}});
            receivingNames.erase(id);
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMediaReceivingCompleted, &app,
        [&](const QString &id, const QString &, const QString &, const QString &type,
            const QString &name, const QByteArray &bytes) {
            if (name.contains('/') || name.contains('\\') || name.contains("..")) { failed = true; return; }
            QSaveFile file(root + "/received/" + name);
            const bool saved = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
            failed = failed || !saved;
            emitEvent("receive_completed", {{"transfer",id},{"name",name},{"type",type},
                {"bytes",bytes.size()},{"saved",saved},
                {"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}});
            receivingNames.erase(id);
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::errorOccurred, &app,
        [&](const QString &title, const QString &) {
            if (rejectedJoinPending && coordinator->state() == MeetingState::Failed &&
                title == QCoreApplication::translate("MeetingUI", "Room Connection Failed")) {
                rejectedJoinPending = false;
                emitEvent("auth_rejected");
            } else { failed = true; emitEvent("coordinator_error"); }
        });
    int sequence = 0;
    QTimer commands;
    commands.setInterval(20);
    QObject::connect(&commands, &QTimer::timeout, &app, [&] {
        if (joinPending && MeetingCoordinatorTestAccess::fileSessionReady(*coordinator)) {
            joinPending = false;
            ++joins;
            rooms.push_back(coordinator->room());
            emitEvent("joined", {{"join", joins}});
            if (board) coordinator->activateWhiteboard();
        }
        if (stateFuture.valid() && stateFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto counts = stateFuture.get();
                counts.insert("sequence", snapshotSequence);
                emitEvent("file_state", counts);
            } catch (...) { failed = true; emitEvent("snapshot_failed"); }
        }
        QFile control(root + "/control.json");
        if (!control.open(QIODevice::ReadOnly)) return;
        const auto command = QJsonDocument::fromJson(control.readAll()).object();
        // Release the Windows file handle before executing a command: the
        // driver may atomically replace the next command after admission.
        control.close();
        const int next = command.value("sequence").toInt();
        if (next <= sequence) return;
        if (command.value("at_ms").toDouble() > QDateTime::currentMSecsSinceEpoch()) return;
        sequence = next;
        const auto action = command.value("action").toString();
        emitEvent("command", {{"sequence",sequence},{"action",action}});
        if (performance && action=="telemetry_stop_sampling") {
            performance->stopSampling(MeetingCoordinatorTestAccess::telemetrySession(*coordinator));
            return;
        }
        if (performance && performance->handle(command)) return;
        if (board && board->handle(command)) return;
        if (network && network->handle(command)) return;
        if (action == "join") join();
        else if (action == "join_rejected" && network &&
                 !qEnvironmentVariable("B_AUTH_REJECTED_TOKEN").isEmpty()) {
            rejectedJoinPending = true;
            coordinator->connectDirectlyAsync(url, qEnvironmentVariable("B_AUTH_REJECTED_TOKEN"),
                roomName, "B File Fixture", preferences);
            rooms.push_back(coordinator->room());
        }
        else if (action == "leave") coordinator->leaveMeetingAsync();
        else if (action == "retirement_observe") {
            int alive = 0;
            for (const auto &room : rooms) if (!room.expired()) ++alive;
            emitEvent("retirement_observation", {{"sequence",sequence},
                {"rooms_observed",static_cast<int>(rooms.size())},{"rooms_alive",alive},
                {"cleanup_pending",static_cast<qint64>(SessionShutdownService::Instance().pending())}});
        }
        else if (action == "snapshot") {
            if (stateFuture.valid()) { failed = true; emitEvent("snapshot_overlap"); return; }
            snapshotSequence = sequence;
            stateFuture = MeetingCoordinatorTestAccess::fileState(*coordinator);
        } else if (action == "send") {
            const auto path = QFileInfo(command.value("path").toString()).canonicalFilePath();
            if (!path.startsWith(QFileInfo(root + "/../payloads").canonicalFilePath() + "/")) {
                failed = true; emitEvent("payload_path_rejected"); return;
            }
            QFile input(path);
            if (!input.open(QIODevice::ReadOnly)) { failed = true; emitEvent("payload_unreadable"); return; }
            const auto bytes = input.readAll();
            const auto id = command.value("message").toString();
            emitEvent("send_requested", {{"message",id},{"bytes",bytes.size()},
                {"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}});
            coordinator->sendChatMediaMessage(id,"file",command.value("name").toString(),bytes);
        } else if (action == "finish") {
            commands.stop();
            SessionShutdownService::Instance().DrainAsync([&] {
                int alive = 0;
                for (const auto &room : rooms) if (!room.expired()) ++alive;
                const auto pending = SessionShutdownService::Instance().pending();
                emitEvent("retired", {{"rooms_observed",static_cast<int>(rooms.size())},
                    {"rooms_alive",alive},{"cleanup_pending",static_cast<qint64>(pending)}});
                app.exit(failed || alive || pending ? 1 : 0);
            });
        } else { failed = true; emitEvent("unknown_command"); }
    });
    commands.start();
    const int telemetrySeconds=qEnvironmentVariableIntValue("B_TELEMETRY_MAX_SECONDS");
    if (performance && (telemetrySeconds<180 || telemetrySeconds>10800)) return 2;
    const bool boundedTelemetryRun=network && telemetrySeconds>=180 && telemetrySeconds<=10800;
    QTimer::singleShot(boundedTelemetryRun ? telemetrySeconds*1000 : 180000,&app,
        [&] { emitEvent("fixture_timeout"); app.exit(2); });
    emitEvent("started", {{"pid",static_cast<qint64>(QCoreApplication::applicationPid())},
        {"identity",identity},{"configuration","RelWithDebInfo"}});
    const auto result = app.exec();
    coordinator.reset();
    return result;
}
