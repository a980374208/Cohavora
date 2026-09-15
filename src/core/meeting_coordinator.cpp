#include "src/core/meeting_coordinator.h"
#include "src/ui/meeting_log_console.h"

#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QMetaObject>

namespace OpenMeeting {

// 从 LiveKit Participant 中提取真实用户昵称 (优先解析 OpenMeeting metadata 中的 JSON userInfo.nickname)
static QString ResolveParticipantNickname(const std::shared_ptr<livekit::Participant> &p) {
    if (!p) return QString();
    QString identity = QString::fromStdString(p->identity()).trimmed();
    QString metaStr = QString::fromStdString(p->metadata()).trimmed();
    if (!metaStr.isEmpty()) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(metaStr.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject obj = doc.object();
            // 1. userInfo.nickname (OpenMeeting 核心标准)
            if (obj.contains("userInfo") && obj["userInfo"].isObject()) {
                QJsonObject userObj = obj["userInfo"].toObject();
                QString nick = userObj["nickname"].toString().trimmed();
                if (!nick.isEmpty()) return nick;
                QString uName = userObj["name"].toString().trimmed();
                if (!uName.isEmpty() && uName != "participant-name") return uName;
            }
            // 2. 根级 nickname
            QString rootNick = obj["nickname"].toString().trimmed();
            if (!rootNick.isEmpty()) return rootNick;
            // 3. 根级 name (排除 participant-name)
            QString rootName = obj["name"].toString().trimmed();
            if (!rootName.isEmpty() && rootName != "participant-name") return rootName;
        }
    }
    // 4. Participant 原生 name (排除 participant-name)
    QString rawName = QString::fromStdString(p->name()).trimmed();
    if (!rawName.isEmpty() && rawName != "participant-name") {
        return rawName;
    }
    // 5. 回退 identity (纯数字 ID)
    return identity;
}

// ----------------------------------------------------
// CoordinatorRoomListener: LiveKit 事件监听与桥接
// ----------------------------------------------------
class MeetingCoordinator::CoordinatorRoomListener : public livekit::RoomListener {
public:
    explicit CoordinatorRoomListener(MeetingCoordinator *c) : _coordinator(c) {}

    void OnConnected() override {
        if (!_coordinator) return;
        QMetaObject::invokeMethod(_coordinator, [this]() {
            _coordinator->setState(MeetingState::InMeeting, QString::fromUtf8("已成功连入会议房间"));
            emit _coordinator->meetingJoinedSuccessfully(_coordinator->_currentMeetingId);

            _coordinator->_participants.clear();

            // 解析并同步 JoinResponse 中的 Metadata
            if (_coordinator->_room) {
                auto joinResp = _coordinator->_room->join_response();
                if (joinResp && joinResp->has_room()) {
                    _coordinator->parseRoomMetadata(joinResp->room().metadata());
                }

                // 注册本端 Participant (必须包含自己)
                _coordinator->ensureLocalParticipant();

                // 同步当前已在房间内的参会人
                auto remotes = _coordinator->_room->remote_participants();
                for (const auto &[sid, p] : remotes) {
                    if (p) {
                        QString pId = QString::fromStdString(p->identity());
                        QString realNick = ResolveParticipantNickname(p);
                        ParticipantInfo info;
                        info.identity = pId;
                        info.name = realNick;
                        info.isLocal = false;
                        info.isHost = (pId == _coordinator->_meetingDetail.hostUserId || pId == _coordinator->_meetingDetail.creatorUserId);
                        info.isAudioMuted = true;
                        info.isVideoEnabled = false;
                        for (const auto &[tsid, pub] : p->tracks()) {
                            if (pub && pub->track()) {
                                if (pub->track()->kind() == livekit::TrackKind::Audio) {
                                    info.isAudioMuted = pub->muted();
                                } else if (pub->track()->kind() == livekit::TrackKind::Video) {
                                    info.isVideoEnabled = !pub->muted();
                                    emit _coordinator->remoteVideoTrackAvailable(pId, pub->track());
                                }
                            }
                        }
                        _coordinator->_participants[pId] = info;
                        emit _coordinator->participantJoined(pId, info.name);
                    }
                }
                _coordinator->updateParticipantListAndNotify();
            }
        }, Qt::QueuedConnection);
    }

    void OnDisconnected(const std::string &reason) override {
        if (!_coordinator) return;
        QString qReason = QString::fromStdString(reason);
        QMetaObject::invokeMethod(_coordinator, [this, qReason]() {
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "DISCONNECTED", QString("与 LiveKit 房间连接断开: %1").arg(qReason));
            if (_coordinator->_state != MeetingState::Leaving && _coordinator->_state != MeetingState::Idle) {
                _coordinator->setState(MeetingState::Idle, qReason);
                emit _coordinator->meetingLeft();
            }
        }, Qt::QueuedConnection);
    }

    void OnReconnecting() override {
        if (!_coordinator) return;
        QMetaObject::invokeMethod(_coordinator, [this]() {
            if (!_coordinator || !_coordinator->_sessionRunning ||
                _coordinator->_state == MeetingState::Leaving ||
                _coordinator->_state == MeetingState::Idle) {
                return;
            }
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "RECONNECTING",
                                    QString::fromUtf8("网络中断，正在恢复音视频连接"));
            _coordinator->setState(MeetingState::Reconnecting,
                                   QString::fromUtf8("网络中断，正在恢复连接..."));
        }, Qt::QueuedConnection);
    }

    void OnReconnected() override {
        if (!_coordinator) return;
        QMetaObject::invokeMethod(_coordinator, [this]() {
            if (!_coordinator || !_coordinator->_sessionRunning ||
                _coordinator->_state == MeetingState::Leaving ||
                _coordinator->_state == MeetingState::Idle) {
                return;
            }

            _coordinator->setState(MeetingState::InMeeting,
                                   QString::fromUtf8("音视频连接已恢复"));
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Connection, "RECONNECTED",
                                    QString::fromUtf8("音视频连接已恢复，重新绑定远端视频轨道"));

            // Soft resume keeps the same Track object and this is idempotent.
            // A full restart may preserve a track SID while recreating Track;
            // VideoRenderSession recognizes that replacement and swaps its
            // subscription before accepting frames from the new source.
            if (!_coordinator->_room) return;
            for (const auto &[participant_sid, participant] : _coordinator->_room->remote_participants()) {
                if (!participant) continue;
                const QString identity = QString::fromStdString(participant->identity());
                for (const auto &[track_sid, publication] : participant->tracks()) {
                    if (publication && publication->track() &&
                        publication->track()->kind() == livekit::TrackKind::Video) {
                        emit _coordinator->remoteVideoTrackAvailable(identity, publication->track());
                    }
                }
            }
        }, Qt::QueuedConnection);
    }

    void OnParticipantConnected(std::shared_ptr<livekit::RemoteParticipant> p) override {
        if (!p || !_coordinator) return;
        QString id = QString::fromStdString(p->identity());
        QString realNick = ResolveParticipantNickname(p);
        QMetaObject::invokeMethod(_coordinator, [this, id, realNick]() {
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "REMOTE_JOIN", QString("参会人加入: %1 (昵称: %2)").arg(id).arg(realNick));
            ParticipantInfo info;
            info.identity = id;
            info.name = realNick;
            info.isLocal = false;
            info.isHost = (id == _coordinator->_meetingDetail.hostUserId || id == _coordinator->_meetingDetail.creatorUserId);
            info.isAudioMuted = true;
            info.isVideoEnabled = false;
            _coordinator->_participants[id] = info;
            _coordinator->updateParticipantListAndNotify();
            emit _coordinator->participantJoined(id, realNick);
        }, Qt::QueuedConnection);
    }

    void OnParticipantMetadataChanged(std::shared_ptr<livekit::Participant> p,
                                      const std::string &old_metadata,
                                      const std::string &new_metadata) override {
        if (!p || !_coordinator) return;
        QString id = QString::fromStdString(p->identity());
        QString realNick = ResolveParticipantNickname(p);
        QMetaObject::invokeMethod(_coordinator, [this, id, realNick]() {
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "METADATA_CHANGED",
                                    QString("参会人 [%1] 元数据更新，刷新昵称为: %2").arg(id).arg(realNick));
            auto it = _coordinator->_participants.find(id);
            if (it != _coordinator->_participants.end()) {
                if (!it->second.isLocal) {
                    it->second.name = realNick;
                }
                _coordinator->updateParticipantListAndNotify();
                emit _coordinator->participantJoined(id, realNick);
            }
        }, Qt::QueuedConnection);
    }

    void OnParticipantDisconnected(std::shared_ptr<livekit::RemoteParticipant> p) override {
        if (!p || !_coordinator) return;
        QString id = QString::fromStdString(p->identity());
        QMetaObject::invokeMethod(_coordinator, [this, id]() {
            MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "REMOTE_LEFT", QString("参会人离开: %1").arg(id));
            _coordinator->_participants.erase(id);
            _coordinator->updateParticipantListAndNotify();
            emit _coordinator->participantLeft(id);

            // 检查是否有该参会人正在发送但未完成的多媒体传输，标记为中断
            std::vector<QString> failedTransfers;
            for (auto it = _coordinator->_inboundMediaTransfers.begin(); it != _coordinator->_inboundMediaTransfers.end(); ) {
                if (it->second.senderIdentity == id) {
                    failedTransfers.push_back(it->first);
                    it = _coordinator->_inboundMediaTransfers.erase(it);
                } else {
                    ++it;
                }
            }
            for (const auto &tId : failedTransfers) {
                emit _coordinator->chatMediaReceivingFailed(tId, QString::fromUtf8("发送方已离会"));
            }
        }, Qt::QueuedConnection);
    }

    void OnTrackSubscribed(std::shared_ptr<livekit::Track> track,
                           std::shared_ptr<livekit::TrackPublication> pub,
                           std::shared_ptr<livekit::RemoteParticipant> p) override {
        if (!track || !p || !_coordinator) return;
        std::string identity = p->identity();

        if (track->kind() == livekit::TrackKind::Video) {
            QMetaObject::invokeMethod(_coordinator, [this, track, identity]() {
                if (_coordinator) {
                    emit _coordinator->remoteVideoTrackAvailable(QString::fromStdString(identity), track);
                }
            }, Qt::QueuedConnection);
        }
    }

    void OnTrackUnsubscribed(std::shared_ptr<livekit::Track> track,
                             std::shared_ptr<livekit::TrackPublication> pub,
                             std::shared_ptr<livekit::RemoteParticipant> p) override {
        if (!track || !p || !_coordinator) return;
        QString id = QString::fromStdString(p->identity());
        bool isVideo = (track->kind() == livekit::TrackKind::Video);
        const std::string track_sid = track->sid();
        QMetaObject::invokeMethod(_coordinator, [this, id, isVideo, track_sid]() {
            if (isVideo) {
                emit _coordinator->remoteVideoTrackUnavailable(id, QString::fromStdString(track_sid));
            }
            auto it = _coordinator->_participants.find(id);
            if (it != _coordinator->_participants.end()) {
                if (isVideo) {
                    it->second.isVideoEnabled = false;
                } else {
                    it->second.isAudioMuted = true;
                }
                _coordinator->updateParticipantListAndNotify();
            }
            emit _coordinator->remoteTrackMuted(id, isVideo, true);
        }, Qt::QueuedConnection);
    }

    void OnTrackMuted(std::shared_ptr<livekit::Participant> participant,
                      std::shared_ptr<livekit::TrackPublication> publication,
                      bool muted) override {
        if (!participant || !publication || !publication->track() || !_coordinator) return;
        QString id = QString::fromStdString(participant->identity());
        bool isVideo = (publication->track()->kind() == livekit::TrackKind::Video);
        QMetaObject::invokeMethod(_coordinator, [this, id, isVideo, muted]() {
            auto it = _coordinator->_participants.find(id);
            if (it != _coordinator->_participants.end()) {
                if (isVideo) {
                    it->second.isVideoEnabled = !muted;
                } else {
                    it->second.isAudioMuted = muted;
                }
                _coordinator->updateParticipantListAndNotify();
            }
            emit _coordinator->remoteTrackMuted(id, isVideo, muted);
        }, Qt::QueuedConnection);
    }

    void OnActiveSpeakersChanged(const std::vector<std::shared_ptr<livekit::Participant>> &speakers) override {
        if (!_coordinator) return;
        QMetaObject::invokeMethod(_coordinator, [this, speakers]() {
            for (auto &[id, p] : _coordinator->_participants) {
                p.isSpeaking = false;
                p.audioLevel = 0.0f;
            }
            for (const auto &sp : speakers) {
                if (sp) {
                    QString spId = QString::fromStdString(sp->identity());
                    auto it = _coordinator->_participants.find(spId);
                    if (it != _coordinator->_participants.end()) {
                        it->second.isSpeaking = true;
                        it->second.audioLevel = sp->audio_level();
                    }
                }
            }
            _coordinator->updateParticipantListAndNotify();
            emit _coordinator->activeSpeakersChanged(speakers);
        }, Qt::QueuedConnection);
    }

    void OnDataReceived(const std::vector<uint8_t> &payload,
                        std::shared_ptr<livekit::RemoteParticipant> participant,
                        const std::string &topic) override {
        if (!_coordinator) return;
        std::string sid = participant ? participant->sid() : "";
        std::string identity = participant ? participant->identity() : "";
        QString name = ResolveParticipantNickname(participant);
        _coordinator->handleDataReceived(payload, sid, identity, name);
    }

private:
    MeetingCoordinator *_coordinator;
};

// ----------------------------------------------------
// MeetingCoordinator 实现
// ----------------------------------------------------

std::shared_ptr<MeetingCoordinator> MeetingCoordinator::create(QObject *parent) {
    return std::make_shared<MeetingCoordinator>(parent);
}

MeetingCoordinator::MeetingCoordinator(QObject *parent)
    : QObject(parent) {
    _localAudioSource = std::make_shared<livekit::AudioSource>(48000, 2);
    _localVideoSource = std::make_shared<livekit::VideoSource>(1280, 720);

    _mediaSendTimer = new QTimer(this);
    connect(_mediaSendTimer, &QTimer::timeout, this, &MeetingCoordinator::processNextMediaSendChunk);
}

MeetingCoordinator::~MeetingCoordinator() {
    stopRoomSession();
}

QString MeetingCoordinator::stateString() const {
    switch (_state) {
        case MeetingState::Idle: return QString::fromUtf8("未就绪/闲置");
        case MeetingState::Validating: return QString::fromUtf8("准入校验中");
        case MeetingState::FetchingCredentials: return QString::fromUtf8("凭据获取中");
        case MeetingState::ConnectingRoom: return QString::fromUtf8("正在连接房间");
        case MeetingState::InMeeting: return QString::fromUtf8("会议进行中");
        case MeetingState::Reconnecting: return QString::fromUtf8("断线重连中");
        case MeetingState::Leaving: return QString::fromUtf8("退出清理中");
        case MeetingState::Failed: return QString::fromUtf8("操作失败");
    }
    return QString::fromUtf8("未知状态");
}

bool MeetingCoordinator::isHost() const {
    QString myUserId = SessionManager::instance().userId();
    if (myUserId.isEmpty()) return false;
    return (myUserId == _meetingDetail.hostUserId || myUserId == _meetingDetail.creatorUserId);
}

void MeetingCoordinator::setState(MeetingState s, const QString &detail) {
    if (_state != s) {
        _state = s;
        emit stateChanged(_state, detail);
    }
}

void MeetingCoordinator::joinMeetingAsync(const QString &meetingId,
                                         const QString &password,
                                         const QString &displayName,
                                         const MediaPreferences &prefs) {
    if (_state != MeetingState::Idle && _state != MeetingState::Failed) {
        emit errorOccurred(QString::fromUtf8("入会错误"), QString::fromUtf8("当前已有正在执行的会议流程，请勿重复加入"));
        return;
    }

    _currentMeetingId = meetingId;
    _currentPassword = password;
    _currentDisplayName = displayName.isEmpty() ? SessionManager::instance().nickname() : displayName;
    _mediaPrefs = prefs;
    _audioMuted = !prefs.enableMicrophone;
    _videoEnabled = prefs.enableVideo;

    _participants.clear();
    ensureLocalParticipant();
    updateParticipantListAndNotify();

    setState(MeetingState::Validating, QString::fromUtf8("正在校验会议准入资格..."));

    // 第一阶段：向后端鉴权校验密码与会议有效性
    auto &http = SessionManager::instance().httpClient();
    http.joinMeeting(meetingId, password, [this, meetingId, &http](bool ok, bool, const HttpError &err) {
        if (!ok) {
            setState(MeetingState::Failed, err.message);
            emit errorOccurred(QString::fromUtf8("入会鉴权失败"),
                               err.message.isEmpty() ? QString::fromUtf8("会议不存在或入会密码错误") : err.message);
            return;
        }

        // 第二阶段：换取 LiveKit 令牌与网关 URL
        setState(MeetingState::FetchingCredentials, QString::fromUtf8("正在换取音视频会话令牌..."));
        http.getMeetingToken(meetingId, [this, meetingId](bool tokenOk, const LiveKitAuthInfo &auth, const HttpError &tokenErr) {
            if (!tokenOk || auth.url.isEmpty() || auth.token.isEmpty()) {
                setState(MeetingState::Failed, tokenErr.message);
                emit errorOccurred(QString::fromUtf8("获取凭据失败"),
                                   tokenErr.message.isEmpty() ? QString::fromUtf8("无法换取 LiveKit 房间访问凭证") : tokenErr.message);
                return;
            }

            // 第三阶段：启动 LiveKit 房间连接与媒体发布
            startRoomSession(auth.url, auth.token);
        });
    });
}

void MeetingCoordinator::createAndJoinQuickMeetingAsync(const QString &title,
                                                       int durationSeconds,
                                                       const MediaPreferences &prefs) {
    if (_state != MeetingState::Idle && _state != MeetingState::Failed) {
        emit errorOccurred(QString::fromUtf8("创建错误"), QString::fromUtf8("当前已有活跃会议流程"));
        return;
    }

    _currentDisplayName = SessionManager::instance().nickname();
    _mediaPrefs = prefs;
    _audioMuted = !prefs.enableMicrophone;
    _videoEnabled = prefs.enableVideo;

    _participants.clear();
    ensureLocalParticipant();
    updateParticipantListAndNotify();

    setState(MeetingState::Validating, QString::fromUtf8("正在创建即时会议..."));

    auto &http = SessionManager::instance().httpClient();
    http.createImmediateMeeting(title, durationSeconds, [this, title](bool ok, const LiveKitAuthInfo &auth, const HttpError &err) {
        if (!ok || auth.url.isEmpty() || auth.token.isEmpty()) {
            setState(MeetingState::Failed, err.message);
            emit errorOccurred(QString::fromUtf8("创建即时会议失败"),
                               err.message.isEmpty() ? QString::fromUtf8("服务端未能分配会议房间") : err.message);
            return;
        }

        _currentMeetingId = auth.meetingId;
        _meetingDetail.meetingId = auth.meetingId;
        _meetingDetail.meetingName = title;
        _meetingDetail.hostUserId = SessionManager::instance().userId();
        _meetingDetail.creatorUserId = SessionManager::instance().userId();
        emit meetingDetailUpdated(_meetingDetail);

        MeetingUI::LogToConsole(MeetingUI::LogCategory::General, "MEETING_ID",
            QString("即时会议创建成功！会议号: %1 (其他参会人可凭此 9 位会议号加入)").arg(auth.meetingId));

        startRoomSession(auth.url, auth.token);
    });
}

void MeetingCoordinator::connectDirectlyAsync(const QString &url,
                                             const QString &token,
                                             const QString &meetingId,
                                             const QString &displayName,
                                             const MediaPreferences &prefs) {
    _currentMeetingId = meetingId;
    _currentDisplayName = displayName;
    _mediaPrefs = prefs;
    _audioMuted = !prefs.enableMicrophone;
    _videoEnabled = prefs.enableVideo;

    _participants.clear();
    ensureLocalParticipant();
    updateParticipantListAndNotify();

    startRoomSession(url, token);
}

void MeetingCoordinator::leaveMeetingAsync(bool endMeetingForAll) {
    if (_state == MeetingState::Idle || _state == MeetingState::Leaving) {
        return;
    }

    setState(MeetingState::Leaving, QString::fromUtf8("正在安全退出会议..."));

    QString mId = _currentMeetingId;
    if (!mId.isEmpty() && SessionManager::instance().isLoggedIn()) {
        auto &http = SessionManager::instance().httpClient();
        if (endMeetingForAll && isHost()) {
            http.endMeeting(mId, [](bool, bool, const HttpError &) {});
        } else {
            http.leaveMeeting(mId, [](bool, bool, const HttpError &) {});
        }
    }

    stopRoomSession();

    setState(MeetingState::Idle, QString::fromUtf8("已退出会议"));
    emit meetingLeft();
}

void MeetingCoordinator::startRoomSession(const QString &url, const QString &token) {
    stopRoomSession(); // 确保前序会话已释放

    setState(MeetingState::ConnectingRoom, QString::fromUtf8("正在建立 WebRTC 连接..."));

    _sessionRunning = true;
    _ioContext = std::make_unique<asio::io_context>();
    _workGuard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(_ioContext->get_executor());

    _room = livekit::Room::Create(_ioContext->get_executor());
    _room->SetLogHandler([](const std::string &cat, const std::string &tag, const std::string &msg) {
        MeetingUI::LogCategory c = MeetingUI::LogCategory::General;
        if (cat == "WEBRTC") c = MeetingUI::LogCategory::WebRTC;
        else if (cat == "SIGNAL") c = MeetingUI::LogCategory::Signal;
        else if (cat == "TRACK") c = MeetingUI::LogCategory::Track;
        else if (cat == "ERROR") c = MeetingUI::LogCategory::Error;
        else if (cat == "MEDIA") c = MeetingUI::LogCategory::Media;
        MeetingUI::LogToConsole(c, QString::fromStdString(tag), QString::fromStdString(msg));
    });

    _roomListener = std::make_shared<CoordinatorRoomListener>(this);
    _room->AddListener(_roomListener);

    // 确保本地音频与视频源就绪（复用已有实例，避免重复创建断开外设采集绑定）
    if (!_localAudioSource) {
        _localAudioSource = std::make_shared<livekit::AudioSource>(48000, 2);
    }
    if (!_localVideoSource) {
        _localVideoSource = std::make_shared<livekit::VideoSource>(1280, 720);
    }

    const std::string urlStr = url.toStdString();
    const std::string tokenStr = token.toStdString();

    _ioThread = std::thread([this, urlStr, tokenStr] {
        livekit::SignalOptions opts;
        opts.auto_subscribe = true;
        opts.connect_timeout = std::chrono::seconds(10);

        asio::co_spawn(*_ioContext, [this, urlStr, tokenStr, opts]() -> asio::awaitable<void> {
            try {
                if (!urlStr.empty() && !tokenStr.empty()) {
                    co_await _room->ConnectAsync(urlStr, tokenStr, opts);

                    auto local = _room->local_participant();
                    if (local) {
                        // 发布本地音频轨
                        _localAudioTrack = livekit::LocalAudioTrack::createLocalAudioTrack("simple_audio", _localAudioSource);
                        _localAudioTrack->set_muted(_audioMuted);
                        co_await local->PublishTrackAsync(_localAudioTrack);
                        MeetingUI::LogToConsole(MeetingUI::LogCategory::Track, "PUBLISH",
                            QString("已发布 LocalAudioTrack (初始状态: %1)").arg(_audioMuted ? "静音" : "开麦"));

                        // 发布本地视频轨
                        livekit::VideoPublishOptions vopts;
                        vopts.video_codec = "vp8";
                        _localVideoTrack = livekit::LocalVideoTrack::createLocalVideoTrack("camera_video", _localVideoSource, livekit::TrackSource::Camera, vopts);
                        _localVideoTrack->set_muted(!_videoEnabled);
                        co_await local->PublishTrackAsync(_localVideoTrack);
                        MeetingUI::LogToConsole(MeetingUI::LogCategory::Track, "PUBLISH",
                            QString("已发布 LocalVideoTrack (VP8, 初始状态: %1)").arg(_videoEnabled ? "开启" : "关闭"));
                    }
                }
            } catch (const std::exception &ex) {
                QString err = QString::fromStdString(ex.what());
                MeetingUI::LogToConsole(MeetingUI::LogCategory::Error, "EXCEPTION", QString("连接房间异常: %1").arg(err));
                QMetaObject::invokeMethod(this, [this, err]() {
                    setState(MeetingState::Failed, err);
                    emit errorOccurred(QString::fromUtf8("连接房间失败"), err);
                }, Qt::QueuedConnection);
            }
        }, asio::detached);

        _ioContext->run();
    });
}

void MeetingCoordinator::stopRoomSession() {
    const bool was_running = _sessionRunning.exchange(false);
    if (!was_running) {
        return;
    }

    if (_mediaSendTimer && _mediaSendTimer->isActive()) {
        _mediaSendTimer->stop();
    }
    for (const auto &task : _mediaSendQueue) {
        if (!task.messageId.isEmpty()) {
            emit chatMessageSendFailed(task.messageId, QString::fromUtf8("会议已退出"));
        }
    }
    _mediaSendQueue.clear();

    for (const auto &[tId, trans] : _inboundMediaTransfers) {
        emit chatMediaReceivingFailed(tId, QString::fromUtf8("本地已离开会议"));
    }
    _inboundMediaTransfers.clear();

    if (_wasapiCap) {
        _wasapiCap->Stop();
        _wasapiCap.reset();
    }
    if (_dshowCap) {
        _dshowCap->Stop();
        _dshowCap.reset();
    }

    if (_room) {
        _room->Disconnect();
    }

    if (_workGuard) {
        _workGuard->reset();
    }
    if (_ioContext) {
        _ioContext->stop();
    }
    if (_ioThread.joinable()) {
        _ioThread.join();
    }

    _roomListener.reset();
    _room.reset();
    _ioContext.reset();
    _workGuard.reset();
    _localAudioTrack.reset();
    _localVideoTrack.reset();
    _localAudioSource.reset();
    _localVideoSource.reset();
}

void MeetingCoordinator::setLocalAudioMuted(bool muted) {
    _audioMuted = muted;
    if (_localAudioTrack) {
        _localAudioTrack->set_muted(muted);
    }
    for (auto &[id, p] : _participants) {
        if (p.isLocal) {
            p.isAudioMuted = muted;
            break;
        }
    }
    updateParticipantListAndNotify();
    emit localAudioMuteChanged(muted);
}

void MeetingCoordinator::setLocalVideoEnabled(bool enabled) {
    _videoEnabled = enabled;
    if (_localVideoTrack) {
        _localVideoTrack->set_muted(!enabled);
    }
    for (auto &[id, p] : _participants) {
        if (p.isLocal) {
            p.isVideoEnabled = enabled;
            break;
        }
    }
    updateParticipantListAndNotify();
    emit localVideoEnableChanged(enabled);
}

void MeetingCoordinator::sendNotifyData(const openmeeting::meeting::NotifyMeetingData &data, bool reliable) {
    if (!_room || _state != MeetingState::InMeeting) return;
    std::string bytes;
    if (!data.SerializeToString(&bytes)) return;
    std::vector<uint8_t> payload(bytes.begin(), bytes.end());
    _room->PublishData(payload, reliable);
}

int64_t MeetingCoordinator::nextSequenceNumber() {
    int64_t now = QDateTime::currentMSecsSinceEpoch() * 1000;
    int64_t counter = ++_msgSequenceCounter;
    return now + (counter % 1000);
}

void MeetingCoordinator::sendChatMessage(const QString &content, const QString &messageId, int64_t seq) {
    if (content.isEmpty()) return;
    if (!_room || _state != MeetingState::InMeeting) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QString::fromUtf8("未连入会议房间"));
        }
        return;
    }
    if (seq <= 0) {
        seq = nextSequenceNumber();
    }

    QJsonObject obj;
    obj["om_type"] = "chat_text";
    obj["seq"] = static_cast<double>(seq);
    obj["msgId"] = messageId;
    obj["text"] = content;

    QJsonDocument doc(obj);
    QByteArray jsonBytes = doc.toJson(QJsonDocument::Compact);
    std::vector<uint8_t> payload(jsonBytes.begin(), jsonBytes.end());
    try {
        bool ok = _room->PublishData(payload, true, {}, "chat");
        if (ok) {
            if (!messageId.isEmpty()) {
                QMetaObject::invokeMethod(this, [this, messageId]() {
                    emit chatMessageSendProgress(messageId, 100);
                    emit chatMessageSendSuccess(messageId);
                }, Qt::QueuedConnection);
            }
        } else {
            if (!messageId.isEmpty()) {
                emit chatMessageSendFailed(messageId, QString::fromUtf8("数据通道拥塞，发送失败"));
            }
        }
    } catch (const std::exception &e) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QString::fromUtf8("发送异常: %1").arg(e.what()));
        }
    } catch (...) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QString::fromUtf8("发送遇到未知异常"));
        }
    }
}

void MeetingCoordinator::sendChatMediaMessage(const QString &messageId, const QString &mediaType, const QString &fileName, const QByteArray &data, int64_t seq) {
    if (data.isEmpty()) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QString::fromUtf8("发送数据为空"));
        }
        return;
    }
    if (!_room || _state != MeetingState::InMeeting) {
        if (!messageId.isEmpty()) {
            emit chatMessageSendFailed(messageId, QString::fromUtf8("未连入会议房间"));
        }
        return;
    }
    if (seq <= 0) {
        seq = nextSequenceNumber();
    }

    QString base64 = QString::fromLatin1(data.toBase64());
    const int chunkSize = 10 * 1024; // 每分片 10KB 字符，确保加上 JSON 协议头后小于 15KB，避免触发 DataStream 二次切片与竞争
    const int totalLen = base64.length();
    const int totalChunks = (totalLen + chunkSize - 1) / chunkSize;
    const QString transferId = QString("media_%1_%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(qrand() % 10000);

    // 1. 发送轻量预告包 media_start 瞬时建立接收端占位 (不等待切片，秒送达)
    QJsonObject startObj;
    startObj["om_type"] = "media_start";
    startObj["transferId"] = transferId;
    startObj["seq"] = static_cast<double>(seq);
    startObj["mediaType"] = mediaType;
    startObj["fileName"] = fileName;
    startObj["totalSize"] = static_cast<qint64>(data.size());
    startObj["totalChunks"] = totalChunks;

    QJsonDocument startDoc(startObj);
    QByteArray startBytes = startDoc.toJson(QJsonDocument::Compact);
    std::vector<uint8_t> startPayload(startBytes.begin(), startBytes.end());
    try {
        _room->PublishData(startPayload, true, {}, "chat");
    } catch (...) {
        // 忽略预告包偶发抛错，后续第一个 chunk 也会兜底建立占位
    }

    // 2. 将数据切片入队由 20ms 平滑流控调度
    MediaSendChunkTask task;
    task.messageId = messageId;
    task.mediaType = mediaType;
    task.fileName = fileName;
    task.transferId = transferId;
    task.totalSize = static_cast<qint64>(data.size());
    task.seq = seq;
    task.currentChunk = 0;
    task.totalChunks = totalChunks;
    task.base64Payload = base64;
    task.chunkSize = chunkSize;

    _mediaSendQueue.push_back(std::move(task));

    if (_mediaSendTimer && !_mediaSendTimer->isActive()) {
        _mediaSendTimer->start(20); // 每 20ms 推送一个分片
    }
}

void MeetingCoordinator::processNextMediaSendChunk() {
    if (_mediaSendQueue.empty()) {
        if (_mediaSendTimer && _mediaSendTimer->isActive()) {
            _mediaSendTimer->stop();
        }
        return;
    }

    if (!_room || _state != MeetingState::InMeeting) {
        while (!_mediaSendQueue.empty()) {
            auto task = _mediaSendQueue.front();
            _mediaSendQueue.pop_front();
            if (!task.messageId.isEmpty()) {
                emit chatMessageSendFailed(task.messageId, QString::fromUtf8("网络已断开"));
            }
        }
        if (_mediaSendTimer && _mediaSendTimer->isActive()) {
            _mediaSendTimer->stop();
        }
        return;
    }

    // === DataChannel 背压流控 (Backpressure Flow Control) ===
    // 检查底层 WebRTC SCTP 待发送缓冲区水位，门限设为 64KB
    // 若当前积压大于 64KB，暂停本轮推送，让底层网络充分排空，彻底避免打爆 SCTP 缓冲区与丢包
    uint64_t buffered = _room->GetDataChannelBufferedAmount(true);
    if (buffered > 64 * 1024) {
        return; // 等待下一个 20ms tick 再次检测
    }

    auto &task = _mediaSendQueue.front();
    int i = task.currentChunk;
    QString chunkStr = task.base64Payload.mid(i * task.chunkSize, task.chunkSize);
    QJsonObject obj;
    obj["om_type"] = "media_chunk";
    obj["transferId"] = task.transferId;
    obj["seq"] = static_cast<double>(task.seq);
    obj["chunkIndex"] = i;
    obj["totalChunks"] = task.totalChunks;
    obj["mediaType"] = task.mediaType;
    obj["fileName"] = task.fileName;
    obj["totalSize"] = task.totalSize;
    obj["chunkData"] = chunkStr;

    QJsonDocument doc(obj);
    QByteArray jsonBytes = doc.toJson(QJsonDocument::Compact);
    std::vector<uint8_t> payload(jsonBytes.begin(), jsonBytes.end());
    bool ok = false;
    try {
        ok = _room->PublishData(payload, true, {}, "chat");
    } catch (const std::exception &e) {
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        if (!msgId.isEmpty()) {
            emit chatMessageSendFailed(msgId, QString::fromUtf8("数据包投递失败: %1").arg(e.what()));
        }
        return;
    } catch (...) {
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        if (!msgId.isEmpty()) {
            emit chatMessageSendFailed(msgId, QString::fromUtf8("数据包投递遇到未知异常"));
        }
        return;
    }

    if (!ok) {
        // 底层 Send 拒绝 (网络缓冲区已满)，不推进 currentChunk，保留给下一轮 tick 重试
        return;
    }

    task.currentChunk++;
    int progress = std::min(100, (task.currentChunk * 100) / task.totalChunks);
    if (!task.messageId.isEmpty()) {
        emit chatMessageSendProgress(task.messageId, progress);
    }

    if (task.currentChunk >= task.totalChunks) {
        QString msgId = task.messageId;
        _mediaSendQueue.pop_front();
        if (!msgId.isEmpty()) {
            emit chatMessageSendSuccess(msgId);
        }
    }
}

void MeetingCoordinator::requestParticipantMute(const QString &targetUserId, bool isVideo, bool mute) {
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(SessionManager::instance().userId().toStdString());

    auto *streamOp = notify.mutable_streamoperatedata();
    auto *op = streamOp->add_operation();
    op->set_userid(targetUserId.toStdString());
    if (isVideo) {
        op->set_cameraonentry(!mute);
    } else {
        op->set_microphoneonentry(!mute);
    }

    sendNotifyData(notify, true);
}

void MeetingCoordinator::requestParticipantCamera(const QString &targetUserId, bool enable) {
    requestParticipantMute(targetUserId, true, !enable);
}

void MeetingCoordinator::requestParticipantMicrophone(const QString &targetUserId, bool enable) {
    requestParticipantMute(targetUserId, false, !enable);
}

void MeetingCoordinator::muteAllParticipants(bool muteMic, bool /*allowSelfUnmute*/) {
    if (!_room || _state != MeetingState::InMeeting) return;
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(SessionManager::instance().userId().toStdString());

    auto *streamOp = notify.mutable_streamoperatedata();
    for (const auto &[uid, info] : _participants) {
        if (!info.isLocal) {
            auto *op = streamOp->add_operation();
            op->set_userid(uid.toStdString());
            op->set_cameraonentry(info.isVideoEnabled);
            op->set_microphoneonentry(!muteMic);
        }
    }

    sendNotifyData(notify, true);
}

void MeetingCoordinator::transferHost(const QString &newHostUserId) {
    if (!_room || _state != MeetingState::InMeeting) return;
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(SessionManager::instance().userId().toStdString());

    auto *hostData = notify.mutable_meetinghostdata();
    hostData->set_userid(newHostUserId.toStdString());
    hostData->set_operatornickname(SessionManager::instance().nickname().toStdString());
    hostData->set_hosttype("host");

    sendNotifyData(notify, true);

    _meetingDetail.hostUserId = newHostUserId;
    for (auto &[id, p] : _participants) {
        p.isHost = (id == newHostUserId);
    }
    updateParticipantListAndNotify();
    emit hostRoleChanged(newHostUserId, SessionManager::instance().nickname());
    emit meetingDetailUpdated(_meetingDetail);
}

void MeetingCoordinator::kickParticipant(const QString &targetUserId, const QString &reason) {
    openmeeting::meeting::NotifyMeetingData notify;
    notify.set_operatoruserid(SessionManager::instance().userId().toStdString());

    auto *kick = notify.mutable_kickoffmeetingdata();
    kick->set_userid(targetUserId.toStdString());
    kick->set_reason(reason.toStdString());
    kick->set_reasoncode(openmeeting::meeting::KickOffReason::Logout);

    sendNotifyData(notify, true);
}

void MeetingCoordinator::ensureLocalParticipant() {
    QString myId = SessionManager::instance().userId();
    if (myId.isEmpty() && _room && _room->local_participant()) {
        myId = QString::fromStdString(_room->local_participant()->identity());
    }
    if (myId.isEmpty()) {
        myId = _currentDisplayName.isEmpty() ? QString("local_user") : _currentDisplayName;
    }

    ParticipantInfo localInfo;
    localInfo.identity = myId;
    QString resolvedNick;
    if (_room && _room->local_participant()) {
        resolvedNick = ResolveParticipantNickname(_room->local_participant());
    }
    if (!resolvedNick.isEmpty() && resolvedNick != myId) {
        localInfo.name = resolvedNick;
    } else if (!_currentDisplayName.isEmpty()) {
        localInfo.name = _currentDisplayName;
    } else if (!SessionManager::instance().nickname().isEmpty()) {
        localInfo.name = SessionManager::instance().nickname();
    } else {
        localInfo.name = myId;
    }
    localInfo.isLocal = true;
    localInfo.isHost = isHost();
    localInfo.isAudioMuted = _audioMuted;
    localInfo.isVideoEnabled = _videoEnabled;

    // 清除旧的可能存在的本地项，避免重复
    for (auto it = _participants.begin(); it != _participants.end();) {
        if (it->second.isLocal) {
            it = _participants.erase(it);
        } else {
            ++it;
        }
    }
    _participants[myId] = localInfo;
}

std::vector<ParticipantInfo> MeetingCoordinator::participants() const {
    std::vector<ParticipantInfo> list;
    list.reserve(_participants.size());
    for (const auto &[_, info] : _participants) {
        list.push_back(info);
    }
    return list;
}

void MeetingCoordinator::updateParticipantListAndNotify() {
    auto list = participants();
    emit participantsUpdated(list);
}

void MeetingCoordinator::parseRoomMetadata(const std::string &metadata) {
    if (metadata.empty()) return;
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(metadata.data(), static_cast<int>(metadata.size())), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;

    QJsonObject root = doc.object();
    QJsonObject detail = root.value("detail").toObject();
    if (detail.isEmpty()) return;

    QJsonObject info = detail.value("info").toObject();
    QJsonObject setting = detail.value("setting").toObject();

    _meetingDetail.meetingId = info.value("meetingID").toString();
    _meetingDetail.meetingName = info.value("meetingName").toString();
    _meetingDetail.creatorUserId = info.value("creatorUserID").toString();
    _meetingDetail.hostUserId = info.value("hostUserID").toString();
    _meetingDetail.startTime = info.value("startTime").toVariant().toLongLong();
    _meetingDetail.endTime = info.value("endTime").toVariant().toLongLong();

    _meetingDetail.disableMicrophoneOnJoin = setting.value("disableMicrophoneOnJoin").toBool();
    _meetingDetail.disableCameraOnJoin = setting.value("disableCameraOnJoin").toBool();
    _meetingDetail.lockMeeting = setting.value("lockMeeting").toBool();
    _meetingDetail.canJoinEarly = setting.value("canParticipantJoinMeetingEarly").toBool(true);

    emit meetingDetailUpdated(_meetingDetail);
}

void MeetingCoordinator::handleDataReceived(const std::vector<uint8_t> &data,
                                           const std::string &participantSid,
                                           const std::string &participantIdentity,
                                           const QString &participantName) {
    openmeeting::meeting::NotifyMeetingData notify;
    if (!notify.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        QString id = !participantIdentity.empty() ? QString::fromStdString(participantIdentity) : QString::fromStdString(participantSid);
        QString name = participantName;
        if (name.isEmpty() || name == id) {
            auto it = _participants.find(id);
            if (it != _participants.end() && !it->second.name.isEmpty()) {
                name = it->second.name;
            } else {
                name = id;
            }
        }

        // 1. 尝试解析为 JSON 消息协议 (chat_text, media_start, media_chunk)
        QJsonParseError jErr;
        QJsonDocument jDoc = QJsonDocument::fromJson(QByteArray::fromRawData(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size())), &jErr);
        if (jErr.error == QJsonParseError::NoError && jDoc.isObject()) {
            QJsonObject jObj = jDoc.object();
            QString omType = jObj.value("om_type").toString();

            // 1.1 文本聊天消息
            if (omType == "chat_text") {
                QString text = jObj.value("text").toString();
                int64_t seq = jObj.value("seq").toVariant().toLongLong();
                if (seq <= 0) seq = QDateTime::currentMSecsSinceEpoch() * 1000;
                QMetaObject::invokeMethod(this, [this, id, name, text, seq]() {
                    emit chatMessageReceived(id, name, text, seq);
                }, Qt::QueuedConnection);
                return;
            }

            // 1.2 多媒体传输轻量预告包 (建立气泡占位)
            if (omType == "media_start") {
                QString transferId = jObj.value("transferId").toString();
                int64_t seq = jObj.value("seq").toVariant().toLongLong();
                if (seq <= 0) seq = QDateTime::currentMSecsSinceEpoch() * 1000;
                int totalChunks = jObj.value("totalChunks").toInt();
                QString mType = jObj.value("mediaType").toString();
                QString fName = jObj.value("fileName").toString();
                qint64 totalSize = jObj.value("totalSize").toVariant().toLongLong();

                auto &transfer = _inboundMediaTransfers[transferId];
                transfer.mediaType = mType;
                transfer.fileName = fName;
                transfer.totalChunks = totalChunks;
                transfer.totalSize = totalSize;
                transfer.seq = seq;
                transfer.senderIdentity = id;
                transfer.senderName = name;
                transfer.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();

                QMetaObject::invokeMethod(this, [this, transferId, id, name, mType, fName, totalSize, seq]() {
                    emit chatMediaReceivingStarted(transferId, id, name, mType, fName, totalSize, seq);
                }, Qt::QueuedConnection);
                return;
            }

            // 1.3 多媒体数据分片
            if (omType == "media_chunk") {
                QString transferId = jObj.value("transferId").toString();
                int chunkIdx = jObj.value("chunkIndex").toInt();
                int totalChunks = jObj.value("totalChunks").toInt();
                QString mType = jObj.value("mediaType").toString();
                QString fName = jObj.value("fileName").toString();
                qint64 totalSize = jObj.value("totalSize").toVariant().toLongLong();
                int64_t seq = jObj.value("seq").toVariant().toLongLong();
                if (seq <= 0) seq = QDateTime::currentMSecsSinceEpoch() * 1000;
                QString chunkData = jObj.value("chunkData").toString();

                bool isFirstChunk = (_inboundMediaTransfers.find(transferId) == _inboundMediaTransfers.end());
                auto &transfer = _inboundMediaTransfers[transferId];
                if (isFirstChunk) {
                    transfer.mediaType = mType;
                    transfer.fileName = fName;
                    transfer.totalChunks = totalChunks;
                    transfer.totalSize = totalSize;
                    transfer.seq = seq;
                    transfer.senderIdentity = id;
                    transfer.senderName = name;
                    transfer.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();

                    QMetaObject::invokeMethod(this, [this, transferId, id, name, mType, fName, totalSize, seq]() {
                        emit chatMediaReceivingStarted(transferId, id, name, mType, fName, totalSize, seq);
                    }, Qt::QueuedConnection);
                }

                transfer.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();
                transfer.receivedChunks[chunkIdx] = chunkData;

                int progress = std::min(99, (static_cast<int>(transfer.receivedChunks.size()) * 100) / totalChunks);
                QMetaObject::invokeMethod(this, [this, transferId, progress]() {
                    emit chatMediaReceivingProgress(transferId, progress);
                }, Qt::QueuedConnection);

                if (static_cast<int>(transfer.receivedChunks.size()) == totalChunks) {
                    QString fullBase64;
                    fullBase64.reserve(totalChunks * 10240);
                    for (int i = 0; i < totalChunks; ++i) {
                        fullBase64.append(transfer.receivedChunks[i]);
                    }
                    QByteArray completeData = QByteArray::fromBase64(fullBase64.toLatin1());
                    _inboundMediaTransfers.erase(transferId);

                    QMetaObject::invokeMethod(this, [this, transferId, id, name, mType, fName, completeData]() {
                        emit chatMediaReceivingProgress(transferId, 100);
                        emit chatMediaReceivingCompleted(transferId, id, name, mType, fName, completeData);
                        emit chatMediaMessageReceived(id, name, mType, fName, completeData);
                    }, Qt::QueuedConnection);
                }
                return;
            }
        }

        // 2. 向下兼容：若不是 JSON 协议，作为普通文本聊天广播
        QString text = QString::fromUtf8(reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()));
        int64_t seq = QDateTime::currentMSecsSinceEpoch() * 1000;
        QMetaObject::invokeMethod(this, [this, id, name, text, seq]() {
            emit chatMessageReceived(id, name, text, seq);
        }, Qt::QueuedConnection);
        return;
    }

    const QString localUserId = SessionManager::instance().userId();

    // 1. KickOff 踢出信令
    if (notify.has_kickoffmeetingdata()) {
        const auto &kick = notify.kickoffmeetingdata();
        QString targetUser = QString::fromStdString(kick.userid());
        if (targetUser == localUserId) {
            QString reason = QString::fromStdString(kick.reason());
            int code = static_cast<int>(kick.reasoncode());
            QMetaObject::invokeMethod(this, [this, reason, code]() {
                MeetingUI::LogToConsole(MeetingUI::LogCategory::Participant, "KICK_OFF", QString("收到踢出信令: %1 (代码: %2)").arg(reason).arg(code));
                emit kickedOff(reason, code);
                leaveMeetingAsync(false);
            }, Qt::QueuedConnection);
            return;
        }
    }

    // 2. StreamOperateData 远端流控信令 (开/关麦、开/关摄)
    if (notify.has_streamoperatedata()) {
        const auto &streamData = notify.streamoperatedata();
        QString opUser = QString::fromStdString(notify.operatoruserid());
        for (int i = 0; i < streamData.operation_size(); ++i) {
            const auto &op = streamData.operation(i);
            if (QString::fromStdString(op.userid()) == localUserId) {
                bool camEnable = op.cameraonentry();
                QMetaObject::invokeMethod(this, [this, camEnable, opUser]() {
                    emit remoteMuteRequested(true, !camEnable, opUser);
                }, Qt::QueuedConnection);

                bool micEnable = op.microphoneonentry();
                QMetaObject::invokeMethod(this, [this, micEnable, opUser]() {
                    emit remoteMuteRequested(false, !micEnable, opUser);
                }, Qt::QueuedConnection);
            }
        }
    }

    // 3. MeetingHostData 主持人角色变更信令
    if (notify.has_meetinghostdata()) {
        const auto &hostData = notify.meetinghostdata();
        QString newHost = QString::fromStdString(hostData.userid());
        QString opNick = QString::fromStdString(hostData.operatornickname());
        QMetaObject::invokeMethod(this, [this, newHost, opNick]() {
            _meetingDetail.hostUserId = newHost;
            for (auto &[id, p] : _participants) {
                p.isHost = (id == newHost);
            }
            updateParticipantListAndNotify();
            emit hostRoleChanged(newHost, opNick);
            emit meetingDetailUpdated(_meetingDetail);
        }, Qt::QueuedConnection);
    }
}

} // namespace OpenMeeting
