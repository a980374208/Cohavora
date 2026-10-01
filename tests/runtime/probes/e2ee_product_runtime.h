#pragma once

#include "src/ui/meeting_room_window.h"
#include "src/ui/meeting_auto_share.h"
#include "src/ui/meeting_encryption_panel.h"
#include "src/core/meeting_coordinator.h"
#include "src/core/session_shutdown_service.h"
#include "src/core/local_video_track.h"
#include "tests/runtime/probes/e2ee_product_lifecycle.h"
#include "tests/runtime/probes/e2ee_camera_correspondence.h"
#include "tests/runtime/probes/e2ee_output_evidence.h"
#include "tests/runtime/probes/e2ee_invitation_evidence.h"
#include "tests/runtime/probes/e2ee_product_byte_stream.h"
#include "src/net/service_endpoint_policy.h"
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QSet>
#include <QtGui/QImage>
#include <algorithm>
#include <future>
#include <QtWidgets/QApplication>
#include "base/platform/win/base_windows_winrt.h"

// Opt-in real product fixture. No fake Connected state, room-start interception,
// custom media cryptor, or direct KeyProvider mutation is used here.
inline int RunE2eeProductRuntime(QApplication& app) {
    using namespace MeetingUI;
    using namespace OpenMeeting;
    // Match main_meeting_app: toolkit WinRT imports must be resolved before
    // screen capture becomes the first WinRT caller on its worker thread.
    if (!base::WinRT::Supported()) return 2;
    const auto directory = qEnvironmentVariable("E2EE_EVIDENCE_DIR");
    if (directory.isEmpty() || qEnvironmentVariableIsEmpty("LIVEKIT_TOKEN")) return 2;
    QFile evidence(directory + "/product.jsonl");
    if (!evidence.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return 2;
    const auto emitEvent = [&](const char* event, QJsonObject data = {}) {
        data.insert("event", QString::fromLatin1(event));
        data.insert("time_ms", QDateTime::currentMSecsSinceEpoch());
        evidence.write(QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n');
        evidence.flush();
    };
    initializeServiceEndpointPolicy(app.arguments().contains("--debug"));
    E2eeOutputEvidence outputArtifacts(directory);
    E2eeInvitationEvidence invitationEvidence;
    if (!outputArtifacts.ready()) return 2;
    const auto identity = qEnvironmentVariable("E2EE_PRODUCT_IDENTITY", "publisher");
    const bool business = qEnvironmentVariable("E2EE_PRODUCT_BUSINESS") == "1";
    const bool whiteboard = qEnvironmentVariable("E2EE_PRODUCT_WHITEBOARD") == "1";
    const bool flutterAuthority = qEnvironmentVariable("E2EE_FLUTTER_AUTHORITY") == "1";
    const bool businessObserver = qEnvironmentVariable("E2EE_PRODUCT_OBSERVER") == "1";
    const bool negativeKey = qEnvironmentVariable("E2EE_PRODUCT_NEGATIVE") == "1";
    const bool camera = qEnvironmentVariable("E2EE_PRODUCT_CAMERA") == "1";
    const bool encrypted = qEnvironmentVariable("E2EE_MODE") != "off";
    const bool steady = qEnvironmentVariable("E2EE_PRODUCT_STEADY") == "1";
    const bool inflight = qEnvironmentVariable("E2EE_PRODUCT_INFLIGHT") == "1";
    bool inflightSent = false, inflightReceived = false;
    QSet<QString> inflightIds;
    const auto codec = qEnvironmentVariable("E2EE_CODEC", "h264");
    SessionManager::instance().loginAsGuest("E2EE Product Fixture", identity);
    MediaPreferences preferences;
    preferences.enableMicrophone = true;
    preferences.enableVideo = camera;
    preferences.cameraVideoCodec = codec;
    preferences.screenShareVideoCodec = codec;
    SessionManager::instance().setMediaPreferences(preferences);
    auto coordinator = MeetingCoordinator::create();
    E2eeProductLifecycle lifecycle(*coordinator, emitEvent);
    E2eeCameraCorrespondence cameraContent(*coordinator, emitEvent);
    E2eeProductByteStream byteStreams(*coordinator, emitEvent);
    MeetingEncryptionDialog input;
    input.findChild<QCheckBox*>("e2eeRequired")->setChecked(encrypted);
    input.findChild<QLineEdit*>("e2eeKeyInput")->setText(qEnvironmentVariable("E2EE_TEST_KEY"));
    input.accept();
    qunsetenv("E2EE_TEST_KEY");
    auto request = input.takeRequest();
    if (!request) return 2;
    MeetingRoomWindow::Config config;
    config.serverUrl = qEnvironmentVariable("LIVEKIT_URL");
    config.token = qEnvironmentVariable("LIVEKIT_TOKEN");
    config.meetingId = qEnvironmentVariable("E2EE_RUN_ID");
    config.displayName = "E2EE Product Fixture";
    config.audioMuted = false;
    config.videoEnabled = camera;
    qunsetenv("LIVEKIT_TOKEN");
    auto window = std::make_unique<MeetingRoomWindow>(config, coordinator);
    bool failed = false, closing = false, recoverySubmitted = false;
    bool reconnectRequested = false;
    QSet<quint64> businessSent;
    QSet<quint64> boardSent, imageSent;
    quint64 currentEpoch = 0;
    bool boardCanEdit = false;
    QString boardActor;
    std::optional<livekit::whiteboard::Document> boardDocument;
    const auto imageBytes = [](quint64 epoch) {
        const auto path = qEnvironmentVariable(QString("E2EE_WHITEBOARD_ASSET%1").arg(epoch).toLatin1().constData());
        if (!path.isEmpty()) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return QByteArray{};
            return file.readAll();
        }
        QImage image(320, 180, QImage::Format_RGB32);
        image.fill(epoch == 4 ? QColor(25, 80, 190) : QColor(190, 80, 25));
        QByteArray bytes;
        QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        return bytes;
    };
    const auto submitBoard = [&] {
        if (!whiteboard || flutterAuthority || identity != "publisher" || !boardCanEdit || !boardDocument
            || (currentEpoch != 4 && currentEpoch != 6) || boardSent.contains(currentEpoch)) return;
        boardSent.insert(currentEpoch);
        livekit::whiteboard::Command command;
        command.id = "e2ee-command-" + std::to_string(currentEpoch);
        command.actor = boardActor.toStdString();
        command.context = boardDocument->context();
        command.kind = livekit::whiteboard::CommandKind::Add;
        command.object.id = "e2ee-object-" + std::to_string(currentEpoch);
        command.object.author = command.actor;
        command.object.kind = livekit::whiteboard::ObjectKind::Text;
        command.object.text = "public e2ee whiteboard " + std::to_string(currentEpoch);
        command.object.points = {{10, 10}, {60, 60}};
        coordinator->submitWhiteboardCommand(command);
        emitEvent("product_whiteboard_submitted", {{"epoch", static_cast<qint64>(currentEpoch)}});
    };
    QObject::connect(coordinator.get(), &MeetingCoordinator::whiteboardProjectionChanged, window.get(),
        [&](const QByteArray& snapshot, quint64, int, const QString&, const QString& actor,
            bool, bool, bool canEdit, bool, const QVariantMap& assets, const QString&) {
            if (!whiteboard) return;
            boardDocument = livekit::whiteboard::Document::fromJson(std::string_view(snapshot.constData(), snapshot.size()));
            boardCanEdit = canEdit; boardActor = actor;
            if (!boardDocument) return;
            for (const quint64 epoch : {4ULL, 6ULL}) {
                bool found = false;
                bool flutterObject = false;
                for (const auto& page : boardDocument->pages()) for (const auto& object : page.objects)
                {
                    if (object.id == "e2ee-object-" + std::to_string(epoch)
                        && object.text == "public e2ee whiteboard " + std::to_string(epoch)) found = true;
                    if (object.id == "e2ee-flutter-object-" + std::to_string(epoch)
                        && object.text == "public flutter whiteboard " + std::to_string(epoch)) flutterObject = true;
                }
                const auto bytes = imageBytes(epoch);
                const auto assetId = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
                bool assetLinked = false;
                for (const auto& page : boardDocument->pages())
                    if (page.backgroundAssetId == assetId.toStdString()) assetLinked = true;
                emitEvent("product_whiteboard_observed", {{"epoch", static_cast<qint64>(epoch)},
                    {"object_valid", found}, {"flutter_object_valid", flutterObject},
                    {"asset_valid", !bytes.isEmpty() && assetLinked && assets.value(assetId).toByteArray() == bytes}});
                if (!flutterAuthority && identity == "publisher" && found && epoch == currentEpoch && !imageSent.contains(epoch)) {
                    imageSent.insert(epoch);
                    coordinator->submitWhiteboardImage(bytes, assetId, 320, 180,
                        QString("e2ee-image-page-%1").arg(epoch), false);
                }
            }
            submitBoard();
        });
    const auto testText = [](quint64 epoch) { return QString("e2ee-public-product-text-%1").arg(epoch); };
    const auto testFile = [](quint64 epoch) { return QByteArray(65536, static_cast<char>('A' + epoch)); };
    const auto sendBusiness = [&](quint64 epoch) {
        if (!business || businessObserver || businessSent.contains(epoch)) return;
        businessSent.insert(epoch);
        coordinator->sendChatMessage(testText(epoch), QString("text-%1").arg(epoch));
        coordinator->sendChatMediaMessage(QString("file-%1").arg(epoch), "file",
            QString("e2ee-public-file-%1").arg(epoch), testFile(epoch));
        emitEvent("product_business_sent", {{"epoch", static_cast<qint64>(epoch)}});
    };
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMessageReceived, window.get(),
        [&](const QString& sender, const QString&, const QString& message, int64_t) {
            if (!business || sender == identity) return;
            const auto epoch = message.section('-', -1).toULongLong();
            emitEvent("product_chat_received", {{"epoch", static_cast<qint64>(epoch)},
                {"valid", (epoch == 4 || epoch == 6) && message == testText(epoch)}});
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMediaReceivingCompleted, window.get(),
        [&](const QString&, const QString& sender, const QString&, const QString& type,
            const QString& name, const QByteArray& bytes) {
            if (inflight && name == "e2ee-inflight") {
                failed = true; emitEvent("product_inflight_completed"); return;
            }
            if (!business || sender == identity) return;
            const auto epoch = name.section('-', -1).toULongLong();
            emitEvent("product_file_received", {{"epoch", static_cast<qint64>(epoch)},
                {"valid", (epoch == 4 || epoch == 6) && type == "file" && bytes == testFile(epoch)},
                {"bytes", bytes.size()}});
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMessageSendFailed, window.get(),
        [&](const QString& id, const QString&) {
            if (inflight && id == "inflight") { emitEvent("product_inflight_send_cancelled"); return; }
            if (business) { failed = true; emitEvent("product_business_send_failed"); }
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMediaReceivingStarted, window.get(),
        [&](const QString& id, const QString&, const QString&, const QString&, const QString& name, qint64, qint64) {
            if (inflight && name == "e2ee-inflight") inflightIds.insert(id);
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMediaReceivingProgress, window.get(),
        [&](const QString& id, int progress) {
            if (inflightIds.contains(id) && progress > 0 && !inflightReceived) {
                inflightReceived = true; emitEvent("product_inflight_partial_received");
            }
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::chatMediaReceivingFailed, window.get(),
        [&](const QString& id, const QString&) {
            if (inflightIds.contains(id)) emitEvent("product_inflight_receive_cancelled");
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::errorOccurred, window.get(),
        [&](const QString&, const QString&) { failed = true; emitEvent("product_error"); });
    QObject::connect(coordinator.get(), &MeetingCoordinator::stateChanged, window.get(),
        [&](MeetingState state, const QString&) {
            emitEvent("product_state", {{"state", static_cast<int>(state)}});
        });
    MeetingUI::ArmAutomaticScreenShare(coordinator.get(), window.get(), [&] {
        emitEvent("product_auto_share_ready");
        coordinator->requestScreenShareSources();
    });
    QObject::connect(coordinator.get(), &MeetingCoordinator::screenShareSourcesReady, window.get(),
        [&](const std::vector<livekit::DesktopSource>& sources) {
            for (const auto& source : sources) {
                if (source.kind == livekit::DesktopSourceKind::Window && source.title == "E2EE Synthetic Source") {
                    lifecycle.setSource(source);
                    coordinator->startScreenShare(source, 15);
                    emitEvent("product_share_requested");
                    return;
                }
            }
            failed = true; emitEvent("product_source_missing");
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::encryptionMediaStatusChanged, window.get(),
        [&](const livekit::MediaEncryptionStatus& status) {
            QJsonArray tracks;
            for (const auto& track : status.tracks) tracks.append(QJsonObject{
                {"binding_id", static_cast<qint64>(track.binding_id)}, {"receiving", track.receiving},
                {"track_id", QString::fromStdString(track.track_id)},
                {"video", track.video}, {"protected", track.protected_after_current_install},
                {"report", static_cast<int>(track.report)}});
            emitEvent("product_media_status", {{"epoch", static_cast<qint64>(status.install_epoch)},
                {"enabled", status.enabled},
                {"generation", static_cast<qint64>(status.native_generation)}, {"tracks", tracks}});
            if (std::any_of(status.tracks.begin(), status.tracks.end(), [](const auto& t) {
                return t.receiving && t.protected_after_current_install;
            })) { currentEpoch = status.install_epoch; submitBoard();
                if (invitationEvidence.needs(currentEpoch))
                    emitEvent("product_invitation_evidence", invitationEvidence.inspect(*window, coordinator->currentMeetingId(), currentEpoch)); }
            // A real authenticated receive after this install is the readiness
            // boundary. Send once per phase; never retry a failed delivery.
            if (business && !businessObserver && (status.install_epoch == 4 || status.install_epoch == 6)
                && !businessSent.contains(status.install_epoch)
                && std::any_of(status.tracks.begin(), status.tracks.end(), [&](const auto& t) {
                    return (t.receiving || (negativeKey && status.install_epoch == 4)) && t.protected_after_current_install;
                })) {
                sendBusiness(status.install_epoch);
            }
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::encryptionKeyRecoveryFinished, window.get(),
        [&](bool installed) { emitEvent("product_recovery_result", {{"installed", installed}}); if (!installed) failed = true; });
    QObject::connect(coordinator.get(), &MeetingCoordinator::localAudioMuteChanged, window.get(),
        [&](bool muted) { emitEvent("product_audio_state", {{"muted", muted}}); });
    QObject::connect(coordinator.get(), &MeetingCoordinator::telemetrySnapshotChanged, window.get(),
        [&](const QVariantMap& snapshot) {
            if (!encrypted && snapshot.value("uniqueRenderSubmits").toLongLong() > 0) {
                // 4 is the public payload phase marker in Off runs, not a key epoch.
                currentEpoch = 4; submitBoard(); sendBusiness(4);
            }
            emitEvent("product_render", {{"submits", snapshot.value("uniqueRenderSubmits").toLongLong()},
                {"bindings", snapshot.value("renderBindings").toLongLong()}});
            QJsonObject metrics;
            for (const auto* name : {"processCpuPercent", "privateBytes", "cpuAvailability", "memoryAvailability",
                "audioJitterBufferAvailability", "audioJitterBufferDelayMs", "audioJitterBufferTargetDelayMs",
                "audioConcealmentAvailability", "audioConcealedRatio", "audioWindowSamples",
                "inboundVideoFramesDecoded", "windowInboundVideoFramesDecoded", "localVideoFirstInjections",
                "localVideoFirstEncodes", "localPublishNoMedia", "videoPublishModes", "outboundVideoCodecs",
                "videoPublishResolvedProfiles", "videoPublishObservedProfiles", "encoderImplementations"})
                metrics.insert(QString::fromLatin1(name), QJsonValue::fromVariant(snapshot.value(QString::fromLatin1(name))));
            metrics.insert("camera_enabled", coordinator->isLocalVideoEnabled());
            QJsonArray publications;
            if (auto room = coordinator->room()) if (auto local = room->local_participant())
                for (const auto& [sid, publication] : local->tracks()) {
                    const auto state = publication->SnapshotState();
                    publications.append(QJsonObject{{"sid", QString::fromStdString(sid)},
                        {"source", static_cast<int>(state.source)}, {"muted", state.muted},
                        {"track_muted", state.track ? state.track->muted() : true}});
                    if (auto video = std::dynamic_pointer_cast<livekit::LocalVideoTrack>(state.track)) {
                        const auto frames = video->frame_diagnostics();
                        emitEvent("product_capture", {{"sid", QString::fromStdString(sid)},
                            {"source", static_cast<int>(state.source)},
                            {"source_available", frames.source_available}, {"rtc_available", frames.rtc_available},
                            {"source_frames", static_cast<qint64>(frames.source_frames)},
                            {"rtc_input_frames", static_cast<qint64>(frames.rtc_input_frames)},
                            {"rtc_output_frames", static_cast<qint64>(frames.rtc_output_frames)}});
                    }
                }
            metrics.insert("publications", publications);
            emitEvent("product_metrics", metrics);
        });
    QObject::connect(coordinator.get(), &MeetingCoordinator::meetingLeft, window.get(), [&] {
        emitEvent("product_closed");
        // Use the production cleanup-queue barrier, not an elapsed-time guess.
        // The window remains alive until this callback exits the application.
        OpenMeeting::SessionShutdownService::Instance().DrainAsync([&] {
            auto retired = lifecycle.retirementEvidence();
            retired.insert("cleanup_jobs_pending", static_cast<qint64>(
                OpenMeeting::SessionShutdownService::Instance().pending()));
            emitEvent("product_retirement", retired);
            const bool clear = retired.value("rooms_observed").toInt() > 0 &&
                retired.value("rooms_alive").toInt() == 0 &&
                retired.value("managers_alive").toInt() == 0 &&
                retired.value("media_observations_alive").toInt() == 0 &&
                retired.value("cleanup_jobs_pending").toInt(-1) == 0;
            app.exit(failed || !clear ? 1 : 0);
        });
    });
    QTimer tick;
    std::future<livekit::RoomStatsReport> statsFuture;
    qint64 nextStatsAt = 0;
    tick.setInterval(250);
    QObject::connect(&tick, &QTimer::timeout, window.get(), [&] {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (!closing) lifecycle.tick(now);
        if (!closing) byteStreams.tick();
        if (!closing) cameraContent.tick();
        if (statsFuture.valid() && statsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                const auto report = statsFuture.get();
                for (const auto& peer : report.reports) {
                    for (const auto& track : peer.inbound_rtp)
                        emitEvent("product_rtp", {{"direction", "rx"}, {"kind", QString::fromStdString(track.kind)},
                            {"track", QString::fromStdString(track.id)}, {"framesDecoded", static_cast<qint64>(track.frames_decoded)},
                            {"framesReceived", static_cast<qint64>(track.frames_received)},
                            {"packets", static_cast<qint64>(track.packets_received)}});
                    for (const auto& track : peer.outbound_rtp)
                        emitEvent("product_rtp", {{"direction", "tx"}, {"kind", QString::fromStdString(track.kind)},
                            {"track", QString::fromStdString(track.id)}, {"framesEncoded", static_cast<qint64>(track.frames_encoded)},
                            {"packets", static_cast<qint64>(track.packets_sent)}});
                }
            } catch (...) { emitEvent("product_stats_failed"); failed = true; }
        }
        if (!closing && !statsFuture.valid() && now >= nextStatsAt && coordinator->state() == MeetingState::InMeeting) {
            if (auto room = coordinator->room()) {
                emitEvent("product_mode", {{"manager_present", bool(room->e2ee_manager())}});
                nextStatsAt = now + 2000;
                statsFuture = std::async(std::launch::async, [room] { return room->GetStatsSync(); });
            }
        }
        if (!closing && !reconnectRequested && qEnvironmentVariable("E2EE_PRODUCT_RECONNECT") == "full"
            && identity == "publisher" && currentEpoch == 4 && QDateTime::currentMSecsSinceEpoch() >=
                qEnvironmentVariable("E2EE_RECONNECT_AT_MS").toLongLong()) {
            reconnectRequested = true;
            emitEvent("product_reconnect_requested");
            coordinator->room()->SimulateScenario(livekit::SimulateScenarioType::FullReconnect);
        }
        const bool rotationReady = qEnvironmentVariable("E2EE_PRODUCT_BOARD_PARTIAL") == "1"
            ? QFile::exists(qEnvironmentVariable("E2EE_KEY_TRIGGER"))
            : QDateTime::currentMSecsSinceEpoch() >= qEnvironmentVariable("E2EE_KEY_ACTION_AT_MS").toLongLong();
        if (!encrypted || steady || closing || recoverySubmitted || !rotationReady) return;
        if (qEnvironmentVariable("E2EE_PRODUCT_BOARD_PARTIAL") == "1" && !lifecycle.partialAssetReceived()) return;
        if (inflight && !inflightSent) {
            inflightSent = true;
            coordinator->sendChatMediaMessage("inflight", "file", "e2ee-inflight", QByteArray(1024 * 1024, 'I'));
            emitEvent("product_inflight_sent");
        }
        // Observe actual partial reception, not a guessed delay, before rotating.
        if (inflight && !inflightReceived) return;
        recoverySubmitted = true;
        auto* button = window->findChild<QPushButton*>("reenterEncryptionKey");
        if (!button || !button->isEnabled()) { failed = true; emitEvent("product_recovery_unavailable"); return; }
        button->click();
        auto* dialog = dynamic_cast<MeetingEncryptionDialog*>(window->findChild<QDialog*>("meetingEncryptionDialog"));
        if (!dialog) { failed = true; emitEvent("product_recovery_dialog_missing"); return; }
        dialog->findChild<QLineEdit*>("e2eeKeyInput")->setText(qEnvironmentVariable("E2EE_NEXT_TEST_KEY"));
        dialog->accept();
        qunsetenv("E2EE_NEXT_TEST_KEY");
        emitEvent("product_recovery_submitted");
    });
    tick.start();
    const int duration = qBound(20, qEnvironmentVariable("E2EE_DURATION").toInt(), 900);
    QTimer::singleShot(duration * 1000, window.get(), [&] {
        closing = true; tick.stop();
        if (qEnvironmentVariable("E2EE_CAMERA_CONTENT") == "1")
            emitEvent("product_camera_content_finished", cameraContent.finish());
        if (qEnvironmentVariable("E2EE_NO_SCREENSHOT") != "1")
            window->grab().save(directory + "/product-window.png");
        emitEvent("product_leave_requested");
        coordinator->leaveMeetingAsync();
        QTimer::singleShot(15000, window.get(), [&] { emitEvent("product_shutdown_timeout"); app.exit(2); });
    });
    window->show();
    window->prepareMediaAndJoin([coordinator, config, preferences, request = std::move(*request)]() mutable {
        coordinator->connectDirectlyAsync(config.serverUrl, config.token, config.meetingId,
            config.displayName, preferences, std::move(request));
    });
    emitEvent("product_started");
    const int result = app.exec();
    window.reset(); coordinator.reset();
    if (outputArtifacts.enabled()) {
        const auto output = outputArtifacts.finish();
        emitEvent("product_output_evidence", output);
        if (!output.value("valid").toBool()) return 1;
    }
    return result;
}
