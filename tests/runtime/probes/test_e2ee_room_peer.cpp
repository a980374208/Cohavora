// E0 compatibility fixture: explicit test-only RTP bindings, NOT product E2EE.
// No producer starts until every local sender has its enabled cryptor attached.
#include "core/room.h"
#include "e2ee/meeting_encryption.h"
#include "core/participant.h"
#include "core/local_video_track.h"
#include "core/local_audio_track.h"
#include "render/owned_i420_frame.h"
#include "tests/support/test_check.h"
#include "telemetry/stats.h"
#include "telemetry/e2e_media_marker.h"
#include "core/remote_track_publication.h"
#include "api/crypto/frame_crypto_transformer.h"
#include "api/make_ref_counted.h"
#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <future>
#include <mutex>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace livekit {
class RoomE2eeInteropTestAccess {
public:
    static void BusinessEpochGate() {
        asio::io_context io;
        auto room = Room::Create(io.get_executor());
        auto keys = MeetingSecretHandle::Create(std::vector<uint8_t>(32, 'a'))->ConsumeProvider();
        room->EnableE2ee({EncryptionType::GCM, keys});
        room->session_generation_.store(7);
        room->installed_session_generation_ = 7;
        room->connection_state_ = ConnectionState::Connected;
        auto receipt = [&] {
            std::lock_guard lock(room->room_mutex_);
            ParticipantEvent event; event.kind = ParticipantEventKind::DataReceived;
            const auto policy = room->e2ee_manager_;
            room->EnqueueParticipantEventLocked(std::move(event), policy,
                policy->data_packet_state().policy_revision);
            return room->participant_events_.back().sender;
        };
        const auto old = receipt();
        TEST_CHECK(old.encryptionCurrent());
        TEST_CHECK(room->RecoverE2eeSharedKey(MeetingSecretHandle::Create(std::vector<uint8_t>(32, 'b')), 7));
        TEST_CHECK(!old.encryptionCurrent());
        const auto current = receipt();
        TEST_CHECK(current.encryptionCurrent() && current.encryption_revision > old.encryption_revision);
        room->session_generation_.store(8);
        TEST_CHECK(!current.encryptionCurrent());
        room->session_generation_.store(7);
        room->EnableE2ee({EncryptionType::GCM, keys});
        TEST_CHECK(!current.encryptionCurrent());
        auto work = asio::make_work_guard(io);
        std::thread worker([&] { io.run(); });
        room->Retire(); work.reset(); worker.join();
        room.reset();
        TEST_CHECK(!old.encryptionCurrent());
    }
    static void AdmissionGate() {
        TEST_CHECK(WebRTCManager::Instance().Initialize());
        for (bool batch : {false, true}) for (bool unsupported : {false, true}) {
            asio::io_context io;
            auto room = Room::Create(io.get_executor());
            unsigned sends = 0;
            room->local_participant_ = std::make_shared<LocalParticipant>("local", "fixture",
                [&](const proto::SignalRequest&) { ++sends; });
            room->enabled_publish_codecs_ = {"h264"};
            KeyProviderOptions key_options; key_options.shared_key = true;
            auto keys = std::make_shared<KeyProvider>(key_options);
            room->EnableE2ee({unsupported ? EncryptionType::CUSTOM : EncryptionType::GCM, keys});
            std::shared_ptr<Track> track;
            if (unsupported) {
                VideoPublishOptions options; options.video_codec = "h264"; options.simulcast = false;
                track = std::make_shared<LocalVideoTrack>("", "fixture", nullptr, TrackSource::Camera, options);
            } else track = std::make_shared<LocalAudioTrack>("", "fixture", nullptr);
            auto request = std::make_shared<proto::SignalRequest>();
            request->mutable_add_track()->set_cid("fixture");
            auto run = [&]() -> asio::awaitable<void> {
                if (batch) co_await room->PublishLocalTracksBatchAsync({{track, request}});
                else co_await room->PublishLocalTrackAsync(track, *request);
            };
            auto completed = asio::co_spawn(io, run(), asio::use_future);
            io.run();
            bool rejected = false;
            try { completed.get(); } catch (const OperationError& error) {
                rejected = true;
                TEST_CHECK(error.code() == (unsupported ? OperationErrorCode::EncryptionCodecUnsupported : OperationErrorCode::EncryptionKeyUnavailable));
                TEST_CHECK(error.codec() == (unsupported ? "h264" : "opus"));
                TEST_CHECK(error.stage() == (unsupported ? "e2ee_codec_admission" : "e2ee_key_admission"));
                TEST_CHECK(!error.retryable());
                TEST_CHECK(error.recovery_action() == (unsupported ? OperationRecoveryAction::SelectSupportedCodec : OperationRecoveryAction::InstallEncryptionKey));
            }
            TEST_CHECK(rejected && sends == 0 && !room->publisher_pc_);
            TEST_CHECK(room->pending_track_publishes_.empty() && room->published_sender_track_ids_.empty());
            TEST_CHECK(room->media_publish_admissions_ == 0);
        }
        WebRTCManager::Instance().Deinitialize();
    }
    static void PolicyGate() {
        asio::io_context context;
        auto room = Room::Create(context.get_executor());
        auto policy = room->AcquireMediaPublishPolicy();
        bool rejected = false;
        try { room->EnableE2ee({}); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected && !room->e2ee_manager());
        policy.reset();
        room->EnableE2ee({});
        auto original = room->e2ee_manager();
        policy = room->AcquireMediaPublishPolicy();
        rejected = false;
        try { room->EnableE2ee({}); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected && room->e2ee_manager() == original);
        policy.reset();
        room->EnableE2ee({});
        TEST_CHECK(room->e2ee_manager() != original);
        auto active = room->e2ee_manager();
        active->SetEnabled(false);
        room->media_publication_policies_[nullptr] = active->AcquireMediaPublishPolicy();
        room->reconnect_active_ = true;
        room->TakePendingOperationsLocked();
        rejected = false;
        try { active->SetEnabled(true); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected && !active->enabled());
        rejected = false;
        try { room->EnableE2ee({}); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected);
        room->reconnect_active_ = false;
        room->TakePendingOperationsLocked();
        active->SetEnabled(true);
        TEST_CHECK(active->enabled());
        room->native_e2ee_policy_ = active->AcquireMediaPublishPolicy();
        rejected = false;
        try { active->SetEnabled(false); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected && active->enabled());
        rejected = false;
        try { room->EnableE2ee({}); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected);
        room->reconnect_active_ = true;
        room->TakePendingOperationsLocked();
        TEST_CHECK(room->native_e2ee_policy_.use_count() != 0);
        room->reconnect_active_ = false;
        room->TakePendingOperationsLocked();
        TEST_CHECK(room->native_e2ee_policy_.use_count() == 0);
        active->SetEnabled(false);
    }
    static uint64_t Generation(Room& room) { return room.session_generation_.load(std::memory_order_acquire); }
    static void RequireChannelGuardWithoutCryptors(Room& room) {
        room.connect_attempt_test_hooks_ = std::make_shared<Room::ConnectAttemptTestHooks>();
        room.connect_attempt_test_hooks_->require_native_media_guard_without_cryptors = true;
    }
    static void MediaStateGate() {
        asio::io_context io;
        auto room = Room::Create(io.get_executor());
        room->EnableE2ee({});
        room->session_generation_.store(7);
        room->installed_session_generation_ = 7;
        room->connection_state_ = ConnectionState::Connected;
        struct Listener : RoomListener {
            std::vector<MediaEncryptionEvent> events;
            void OnMediaEncryptionStateChanged(const MediaEncryptionEvent& event) override {
                events.push_back(event);
            }
        };
        auto listener = std::make_shared<Listener>(); room->AddListener(listener);
        auto binding = std::make_shared<E2eeManager::MediaObservation>();
        binding->identity = "state-peer"; binding->track = "TR_STATE";
        binding->generation = 7; binding->receiving = true;
        auto active = std::make_shared<std::atomic<bool>>(true); binding->binding_active = active;
        auto drain = [&] { io.restart(); io.poll(); };
        auto emit = [&](EncryptionState state) {
            room->PostMediaEncryptionState(room->e2ee_manager(), binding, state);
        };
        emit(EncryptionState::OK);
        TEST_CHECK(listener->events.empty()); // Never delivered on the crypto caller.
        drain();
        TEST_CHECK(listener->events.size() == 1);
        TEST_CHECK(listener->events.back().track_sid == "TR_STATE");
        TEST_CHECK(listener->events.back().native_track_id.empty());
        emit(EncryptionState::KEY_RATCHETED); drain();
        TEST_CHECK(listener->events.back().state == EncryptionState::KEY_RATCHETED);
        emit(EncryptionState::OK); binding->retired.store(true); drain();
        TEST_CHECK(listener->events.size() == 2);
        binding->retired.store(false);
        emit(EncryptionState::OK); active->store(false); drain();
        TEST_CHECK(listener->events.size() == 2); active->store(true);
        emit(EncryptionState::OK); room->session_generation_.store(8); drain();
        TEST_CHECK(listener->events.size() == 2); room->session_generation_.store(7);
        emit(EncryptionState::OK); room->EnableE2ee({}); drain();
        TEST_CHECK(listener->events.size() == 2);
        binding->receiving = false; binding->track = "native-sender-id";
        emit(EncryptionState::MISSING_KEY); drain();
        TEST_CHECK(listener->events.size() == 3);
        TEST_CHECK(listener->events.back().native_track_id == "native-sender-id");
        TEST_CHECK(listener->events.back().track_sid.empty());
        room->RemoveListener(listener);
        auto work = asio::make_work_guard(io); io.restart();
        std::thread worker([&] { io.run(); });
        room->Retire(); work.reset(); worker.join();
    }
    static void MetadataGate() {
        asio::io_context io;
        auto room = Room::Create(io.get_executor());
        auto keys = std::make_shared<KeyProvider>();
        room->EnableE2ee({EncryptionType::GCM, keys});
        room->connection_state_ = ConnectionState::Connected;
        proto::ParticipantUpdate update;
        auto* remote = update.add_participants();
        remote->set_sid("PA_METADATA"); remote->set_identity("metadata-peer");
        remote->set_state(proto::ParticipantInfo::ACTIVE);
        auto* track = remote->add_tracks(); track->set_sid("TR_METADATA");
        track->set_type(proto::TrackType::VIDEO);
        track->set_encryption(proto::Encryption::NONE);
        room->UpdateParticipantsForTesting(update);
        auto participant = room->remote_participants_.at("PA_METADATA");
        auto publication = participant->get_remote_publication("TR_METADATA");
        TEST_CHECK(publication && publication->encryption() == TrackEncryption::None);
        TEST_CHECK(publication->subscription_error() == TrackPublication::SubscriptionError::EncryptionRequired);
        room->ClearTrackSubscriptionErrorLocked(participant, publication);
        TEST_CHECK(publication->subscription_error() == TrackPublication::SubscriptionError::EncryptionRequired);
        track->set_encryption(proto::Encryption::GCM);
        room->UpdateParticipantsForTesting(update);
        TEST_CHECK(publication->encryption() == TrackEncryption::Gcm);
        TEST_CHECK(publication->subscription_error() == TrackPublication::SubscriptionError::None);
        track->set_encryption(proto::Encryption::CUSTOM);
        room->UpdateParticipantsForTesting(update);
        TEST_CHECK(publication->encryption() == TrackEncryption::Custom);
        TEST_CHECK(publication->subscription_error() == TrackPublication::SubscriptionError::EncryptionRequired);
        // Retirement drains callback leases; its executor must remain running.
        auto work = asio::make_work_guard(io);
        std::thread worker([&] { io.run(); });
        room->Retire();
        work.reset(); worker.join();
    }
    static auto Connections(Room& room) {
        std::lock_guard lock(room.room_mutex_);
        return std::pair(room.publisher_pc_, room.subscriber_pc_);
    }
};
}
namespace {
using namespace std::chrono_literals;
using Json = nlohmann::json;
unsigned fixtureGeneration = 1; // Set between fully retired Room instances only.
std::string Env(const char* name, std::string fallback = {}) {
    const auto* value = std::getenv(name);
    return value ? value : fallback;
}
LONG WINAPI CrashStack(EXCEPTION_POINTERS* error) {
    std::ofstream out(Env("E2EE_EVIDENCE_DIR") + "/native-crash-stack.txt");
    out << "exception_code=" << std::hex << error->ExceptionRecord->ExceptionCode << '\n';
    auto process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *error->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC = {context.Rip, 0, AddrModeFlat};
    frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
    frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
    for (unsigned i = 0; i != 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(),
            &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i) {
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 offset = 0;
        out << frame.AddrPC.Offset;
        if (SymFromAddr(process, frame.AddrPC.Offset, &offset, symbol)) out << ' ' << symbol->Name << '+' << offset;
        out << '\n';
    }
    out.flush();
    return EXCEPTION_EXECUTE_HANDLER;
}
struct Evidence {
    std::mutex mutex;
    std::ofstream file;
    std::string run = Env("E2EE_RUN_ID");
    const unsigned generation = fixtureGeneration;
    std::atomic<unsigned> key_epoch{0};
    std::atomic<int> key_index{0};
    std::atomic<uint64_t> native_generation{0};
    explicit Evidence(const std::string& path) : file(path, std::ios::app) {
        if (!file) throw std::runtime_error("evidence_open_failed");
    }
    void Emit(std::string event, Json fields = Json::object()) {
        std::lock_guard lock(mutex);
        fields["event"] = event; fields["run"] = run;
        fields["generation"] = generation; fields["key_epoch"] = key_epoch.load(); fields["key_index"] = key_index.load();
        fields["generation_scope"] = "fixture_room_incarnation";
        fields["native_generation"] = native_generation.load();
        fields["time_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        file << fields.dump() << '\n'; file.flush();
    }
};
struct Counts {
    std::atomic<uint64_t> input{0}, output{0};
    // Bounded metadata only: never retain encoded frame bytes or arbitrary strings.
    std::atomic<unsigned> codecs{0};
    Json ObservedCodecs() const {
        Json result = Json::array();
        const auto mask = codecs.load();
        const char* names[] = {"audio/opus", "video/VP8", "video/VP9", "video/H264", "video/AV1", "unknown"};
        for (unsigned i = 0; i < 6; ++i) if (mask & (1u << i)) result.push_back(names[i]);
        return result;
    }
};
class CountedSink : public webrtc::TransformedFrameCallback {
public:
    CountedSink(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> next,
                std::shared_ptr<Counts> counts) : next_(std::move(next)), counts_(std::move(counts)) {}
    void OnTransformedFrame(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
        ++counts_->output; next_->OnTransformedFrame(std::move(frame));
    }
private:
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> next_;
    std::shared_ptr<Counts> counts_;
};
class CountedTransform : public webrtc::FrameTransformerInterface {
public:
    CountedTransform(webrtc::scoped_refptr<webrtc::FrameCryptorTransformer> cryptor,
                     std::shared_ptr<Counts> counts) : backend_(cryptor), counts_(std::move(counts)) {}
    void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
        const auto mime = frame->GetMimeType();
        const unsigned bit = mime == "audio/opus" ? 0 : mime == "video/VP8" ? 1 :
            mime == "video/VP9" ? 2 : mime == "video/H264" ? 3 : mime == "video/AV1" ? 4 : 5;
        counts_->codecs.fetch_or(1u << bit, std::memory_order_relaxed);
        ++counts_->input; backend_->Transform(std::move(frame));
    }
    void RegisterTransformedFrameCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> sink) override {
        backend_->RegisterTransformedFrameCallback(webrtc::make_ref_counted<CountedSink>(sink, counts_));
    }
    void RegisterTransformedFrameSinkCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> sink, uint32_t ssrc) override {
        backend_->RegisterTransformedFrameSinkCallback(webrtc::make_ref_counted<CountedSink>(sink, counts_), ssrc);
    }
    void UnregisterTransformedFrameCallback() override { backend_->UnregisterTransformedFrameCallback(); }
    void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override { backend_->UnregisterTransformedFrameSinkCallback(ssrc); }
private:
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> backend_;
    std::shared_ptr<Counts> counts_;
};
class Observer : public webrtc::FrameCryptorTransformerObserver {
public:
    Observer(std::shared_ptr<Evidence> out, std::string track, std::string direction)
        : out_(std::move(out)), track_(std::move(track)), direction_(std::move(direction)) {}
    void OnFrameCryptionStateChanged(const std::string, webrtc::FrameCryptionState state) override {
        out_->Emit("crypto_state", {{"track", track_}, {"direction", direction_}, {"state_code", static_cast<int>(state)}});
    }
private:
    std::shared_ptr<Evidence> out_; std::string track_, direction_;
};
struct Binding {
    std::string id, kind, direction;
    std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    webrtc::scoped_refptr<webrtc::FrameCryptorTransformer> cryptor;
    webrtc::scoped_refptr<CountedTransform> transform;
};
class Peer : public livekit::RoomListener, public std::enable_shared_from_this<Peer> {
public:
    explicit Peer(asio::io_context& io) : strand_(asio::make_strand(io)), timer_(strand_),
        evidence_(std::make_shared<Evidence>(Env("E2EE_EVIDENCE_DIR") + "/native.jsonl")),
        room_(livekit::Room::Create(strand_)) {}
    void Start() {
        room_->AddListener(shared_from_this());
        asio::co_spawn(strand_, Run(), [self = shared_from_this()](std::exception_ptr error) {
            if (error) self->evidence_->Emit("fatal", {{"code", "unhandled_operation"}});
            self->done.set_value();
        });
    }
    void OnTrackSubscribed(std::shared_ptr<livekit::Track> track,
        std::shared_ptr<livekit::TrackPublication>, std::shared_ptr<livekit::RemoteParticipant> participant) override {
        asio::post(strand_, [self = shared_from_this(), track, participant] {
            if (self->closing_) return;
            auto [publisher, subscriber] = livekit::RoomE2eeInteropTestAccess::Connections(*self->room_);
            bool found = false;
            for (auto pc : {subscriber, publisher}) {
                if (!pc || found) continue;
                for (auto receiver : pc->GetReceivers()) {
                    if (receiver->track() != track->rtc_track()) continue;
                    found = true; break;
                }
            }
            self->evidence_->Emit("subscribed", {{"track", track->sid()}, {"receiver_found", found},
                {"kind", track->kind() == livekit::TrackKind::Video ? "video" : "audio"}});
            if (track->kind() == livekit::TrackKind::Video) {
                self->video_subs_.push_back(track->subscribeI420VideoFrames([weak = self->weak_from_this()](auto frame) {
                    if (auto owner = weak.lock()) {
                        const auto decoded = ++owner->decoded_video_;
                        if (owner->verify_relay_marker_) {
                            const auto marker = livekit::telemetry::DecodeE2eMediaMarker(*frame);
                            const bool correct = marker && marker->probe_id == kRelayMarkerProbe;
                            if (decoded == 1) owner->first_marker_correct_ = correct;
                            if (!correct) ++owner->invalid_markers_;
                            else {
                                ++owner->valid_markers_;
                                const auto previous = owner->last_marker_sequence_.exchange(marker->sequence);
                                if (marker->sequence > previous) ++owner->marker_advances_;
                                if (marker->sequence < previous) ++owner->invalid_markers_;
                            }
                        }
                        owner->decoded_width_ = frame->width();
                        owner->decoded_height_ = frame->height();
                    }
                }));
            } else {
                track->addAudioSink([weak = self->weak_from_this()](const livekit::AudioFrame& frame) {
                    if (auto owner = weak.lock()) owner->decoded_audio_samples_ += frame.totalSamples();
                });
            }
        });
    }
    bool finished = false;
    std::promise<void> done;
    void Retire() { room_->Retire(); evidence_->Emit("closed"); }
    void OnMediaEncryptionStateChanged(const livekit::MediaEncryptionEvent& event) override {
        evidence_->Emit("product_crypto_state", {{"identity", event.participant_identity},
            {"track_sid", event.track_sid}, {"native_track_id", event.native_track_id},
            {"binding_generation", event.generation},
            {"observed_room_generation", livekit::RoomE2eeInteropTestAccess::Generation(*room_)},
            {"direction", event.receiving ? "rx" : "tx"},
            {"kind", event.video ? "video" : "audio"}, {"state", static_cast<int>(event.state)}});
    }
    void OnReconnecting() override { evidence_->Emit("reconnecting"); }
    void OnReconnected() override {
        RegisterRpcHandler();
        evidence_->native_generation = livekit::RoomE2eeInteropTestAccess::Generation(*room_);
        evidence_->Emit("reconnected");
    }
    void OnDataReceived(const std::vector<uint8_t>& data, std::shared_ptr<livekit::RemoteParticipant>,
        const std::string& topic) override {
        if (topic != "e2ee.probe") return;
        evidence_->Emit("data_received", {{"content_valid", std::string(data.begin(), data.end()) == "e2ee-probe-public-test-payload"}});
    }
    void OnTextStreamOpened(std::shared_ptr<livekit::TextStreamReader> reader, std::shared_ptr<livekit::Participant>) override {
        asio::post(strand_, [self = shared_from_this(), reader] {
            if (!self->closing_ && self->readers_.size() < 32) self->readers_.push_back(reader);
        });
    }
private:
    webrtc::scoped_refptr<CountedTransform> MakeBinding(const std::string& sid, const std::string& identity,
        livekit::TrackKind kind, const std::string& direction) {
        Binding binding; binding.id = sid; binding.direction = direction;
        binding.kind = kind == livekit::TrackKind::Video ? "video" : "audio";
        binding.cryptor = new webrtc::FrameCryptorTransformer(livekit::WebRTCManager::Instance().signaling_thread(),
            identity, kind == livekit::TrackKind::Video ? webrtc::FrameCryptorTransformer::MediaType::kVideoFrame
                : webrtc::FrameCryptorTransformer::MediaType::kAudioFrame,
            webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, keys_);
        binding.cryptor->SetKeyIndex(evidence_->key_index.load()); binding.cryptor->SetEnabled(true);
        binding.cryptor->RegisterFrameCryptorTransformerObserver(webrtc::make_ref_counted<Observer>(evidence_, sid, direction));
        binding.transform = webrtc::make_ref_counted<CountedTransform>(binding.cryptor, binding.counts);
        bindings_.push_back(binding); return binding.transform;
    }
    asio::awaitable<void> Publish(const std::shared_ptr<livekit::Track>& source,
        const livekit::VideoPublishOptions* video, const livekit::AudioPublishPolicy* audio) {
        auto request = room_->local_participant()->BuildTrackPublishRequest(source, video, audio);
        auto publication = co_await room_->PublishLocalTrackAsync(source, request);
        evidence_->Emit("published", {{"track", publication->sid()},
            {"kind", source->kind() == livekit::TrackKind::Video ? "video" : "audio"},
            {"sender_path", "normal_product_publish_plan_and_pre_negotiation_binding"}});
    }
    void Sample() {
        if (auto manager = room_->e2ee_manager()) {
            for (const auto& observation : manager->media_observations()) {
                const auto install_epoch = observation->install_epoch->load(std::memory_order_acquire);
                const bool binding_active = !observation->retired.load(std::memory_order_acquire) &&
                    (!observation->binding_active || observation->binding_active->load(std::memory_order_acquire));
                const bool protected_current = binding_active && !(install_epoch & 1) &&
                    observation->frame_evidence.protected_epoch() == install_epoch &&
                    observation->install_epoch->load(std::memory_order_acquire) == install_epoch;
                Json codecs = Json::array();
                const char* names[] = {"audio/opus", "video/VP8", "video/VP9", "video/H264", "unknown"};
                for (unsigned i = 0; i < 5; ++i) if (observation->codec_mask.load() & (1u << i)) codecs.push_back(names[i]);
                evidence_->Emit("crypto_counts", {{"track", observation->track}, {"kind", observation->video ? "video" : "audio"},
                    {"direction", observation->receiving ? "rx" : "tx"}, {"input", observation->input.load()}, {"output", observation->output.load()},
                    {"observed_codecs", codecs}, {"codec_measurement", "product_transform_input_mime"},
                    {"binding_generation", observation->generation}, {"state_code", observation->state.load()},
                    {"binding_id", observation->binding_id}, {"binding_active", binding_active},
                    {"install_epoch", install_epoch}, {"protected_after_current_install", protected_current},
                    {"protection_measurement", "non_sif_frame_completion_same_explicit_install_epoch"}});
            }
        }

        for (const auto& binding : bindings_) evidence_->Emit("crypto_counts", {{"track", binding.id},
            {"kind", binding.kind}, {"direction", binding.direction}, {"input", binding.counts->input.load()},
            {"output", binding.counts->output.load()}, {"observed_codecs", binding.counts->ObservedCodecs()},
            {"codec_measurement", "transform_input_mime"}});
        evidence_->Emit("decode_counts", {{"video_frames", decoded_video_.load()}, {"audio_samples", decoded_audio_samples_.load()},
            {"decoded_width", decoded_width_.load()}, {"decoded_height", decoded_height_.load()},
            {"relay_marker_valid", valid_markers_.load()}, {"relay_marker_invalid", invalid_markers_.load()},
            {"relay_marker_advances", marker_advances_.load()}, {"relay_first_frame_correct", first_marker_correct_.load()}});
        for (auto it = readers_.begin(); it != readers_.end();) {
            if (!(*it)->is_closed()) { ++it; continue; }
            if ((*it)->is_failed()) evidence_->Emit("stream_failed");
            else evidence_->Emit("stream_received", {{"content_valid", (*it)->ReadAll() == "e2ee-probe-public-test-stream"}});
            it = readers_.erase(it);
        }
    }
    bool rpc_started_ = false;
    void RegisterRpcHandler() {
        if (auto local = room_->local_participant()) {
            local->registerRpcMethod("e2ee.echo", [weak = weak_from_this()](const livekit::RpcInvocationData& data) -> asio::awaitable<std::string> {
                auto self = weak.lock();
                const bool valid = data.payload == "e2ee-public-rpc-request";
                if (self) self->evidence_->Emit("rpc_handler", {{"content_valid", valid}});
                if (!valid) throw livekit::RpcError(livekit::RpcErrorCode::REJECTED, "unexpected test payload");
                co_return "e2ee-public-rpc-response";
            });
        }
    }
    void MaybeRpc() {
        if (Env("E2EE_RPC") != "1" || rpc_started_ || room_->remote_participants().empty()) return;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (now < std::stoll(Env("E2EE_RPC_AT_MS"))) return;
        rpc_started_ = true;
        const auto destination = room_->remote_participants().begin()->second->identity();
        livekit::safe_co_spawn(strand_, [self = shared_from_this(), destination]() -> asio::awaitable<void> {
            try {
                const auto result = co_await self->room_->local_participant()->performRpc(destination,
                    "e2ee.echo", "e2ee-public-rpc-request", 5.0);
                self->evidence_->Emit("rpc_response", {{"content_valid", result == "e2ee-public-rpc-response"}});
            } catch (...) { self->evidence_->Emit("rpc_failed"); }
        });
    }
    void SendData() {
        if (room_->connection_state() != livekit::ConnectionState::Connected || room_->remote_participants().empty()) return;
        const std::string payload = "e2ee-probe-public-test-payload";
        if (!room_->PublishData({payload.begin(), payload.end()}, true, {}, "e2ee.probe")) {
            evidence_->Emit("data_send_failed"); return;
        }
        try {
            auto writer = room_->CreateTextStreamWriter("e2ee.stream");
            writer->Write("e2ee-probe-public-test-stream"); writer->Close();
            evidence_->Emit("data_sent", {{"user_path", "product_publish_data"}, {"stream_path", "product_writer"}});
        } catch (...) { evidence_->Emit("stream_send_failed"); }
    }
    void MaybeChangeKey() {
        const auto action = Env("E2EE_KEY_ACTION", "none");
        if (!enabled_ || action == "none") return;
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        const int epoch = evidence_->key_epoch.load();
        const int rounds = std::stoi(Env("E2EE_KEY_ROUNDS", "1"));
        if (epoch >= rounds) {
            // A failed participant handler invalidates all its slots. Once
            // both senders have selected slot 1, reinstall its known key via
            // the supported provider API to reset the invalid-key latch.
            if (!receive_key_reinstalled_ && Env("E2EE_RECOVER_INVALID_KEY") == "1" &&
                now >= std::stoll(Env("E2EE_KEY_ACTION_AT_MS")) + 5000) {
                const auto next = Env("E2EE_NEXT_TEST_KEY");
                if (!keys_->SetSharedKey(1, {next.begin(), next.end()})) throw std::runtime_error("key_reinstall_failed");
                receive_key_reinstalled_ = true;
                evidence_->Emit("receive_key_reinstalled", {{"slot", 1}});
            }
            return;
        }
        if (now < std::stoll(Env("E2EE_KEY_ACTION_AT_MS")) + epoch * 10000LL) return;
        Sample();
        if (action == "slot") {
            for (auto& binding : bindings_) if (binding.direction == "tx") binding.cryptor->SetKeyIndex(1);
            if (!room_->e2ee_manager()->SetDataPacketKeyIndex(1)) throw std::runtime_error("send_slot_failed");
            if (!room_->e2ee_manager()->SetMediaKeyIndex(1)) throw std::runtime_error("media_slot_failed");
            evidence_->key_index = 1; evidence_->key_epoch = 1;
            evidence_->Emit("key_changed", {{"action", action}, {"scope", "preinstalled_receive_slot_then_sender_selection"}});
            return;
        }
        std::vector<uint8_t> material;
        if (action == "ratchet") {
            material = packet_keys_->RatchetSharedKey(0);
            if (material.empty() || !keys_->SetSharedKey(0, material)) throw std::runtime_error("ratchet_failed");
        }
        else {
            const auto next = Env("E2EE_NEXT_TEST_KEY") +
                (rounds > 1 ? "-" + std::to_string(epoch + 1) : "");
            material.assign(next.begin(), next.end());
            if (material.empty() || !keys_->SetSharedKey(0, material)) throw std::runtime_error("key_change_failed");
        }
        if (material.empty()) throw std::runtime_error("key_change_failed");
        packet_keys_->SetSharedKey(material);
        evidence_->key_epoch = epoch + 1;
        evidence_->Emit("key_changed", {{"action", action}, {"scope", "fixture_backend_and_packet_provider"}});
    }
    asio::awaitable<void> Run() {
        evidence_->Emit("starting", {{"scope", "explicit_fixture_bindings"}, {"mode", enabled_ ? "required" : "off"}});
        try {
            const bool guard_only = Env("E2EE_NATIVE_CHANNEL_GUARD_ONLY") == "1";
            if (guard_only) {
                TEST_CHECK(!enabled_);
                livekit::RoomE2eeInteropTestAccess::RequireChannelGuardWithoutCryptors(*room_);
            }
            webrtc::KeyProviderOptions options;
            options.shared_key = true;
            const std::string salt = "LKFrameEncryptionKey";
            options.ratchet_salt.assign(salt.begin(), salt.end());
            options.ratchet_window_size = std::stoi(Env("E2EE_RATCHET_WINDOW", "16")); options.key_ring_size = 16;
            options.failure_tolerance = std::stoi(Env("E2EE_FAILURE_TOLERANCE", "-1"));
            keys_ = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(options);
            keys_->options().discard_frame_when_cryptor_not_ready = true;
            if (Env("E2EE_KEY_STATE") != "missing") {
                const auto key = Env("E2EE_TEST_KEY");
                if (key.empty() || !keys_->SetSharedKey(0, {key.begin(), key.end()})) throw std::runtime_error("key_missing");
            }
            if (enabled_) {
                livekit::KeyProviderOptions packetOptions; packetOptions.shared_key = true;
                packetOptions.ratchet_window_size = options.ratchet_window_size;
                packetOptions.failure_tolerance = options.failure_tolerance;
                packet_keys_ = std::make_shared<livekit::KeyProvider>(packetOptions);
                const auto key = Env("E2EE_TEST_KEY");
                if (Env("E2EE_KEY_STATE") != "missing") packet_keys_->SetSharedKey({key.begin(), key.end()});
                livekit::E2eeOptions encryption; encryption.key_provider = packet_keys_;
                room_->EnableE2ee(encryption);
                if (Env("E2EE_KEY_ACTION") == "slot") {
                    const auto next = Env("E2EE_NEXT_TEST_KEY");
                    if (next.empty() || !keys_->SetSharedKey(1, {next.begin(), next.end()})) throw std::runtime_error("prepare_slot_failed");
                    packet_keys_->SetSharedKey({next.begin(), next.end()}, 1);
                    evidence_->Emit("receive_slot_prepared", {{"prepared_slot", 1}});
                }
            }
            livekit::SignalOptions signal;
            signal.auto_subscribe = true; signal.allow_insecure_transport = Env("LIVEKIT_TEST_ALLOW_INSECURE") == "1";
            co_await room_->ConnectAsync(Env("LIVEKIT_URL"), Env("LIVEKIT_TOKEN"), signal);
            evidence_->native_generation = livekit::RoomE2eeInteropTestAccess::Generation(*room_);
            {
                auto [publisher, subscriber] = livekit::RoomE2eeInteropTestAccess::Connections(*room_);
                const auto required = [](const auto& pc) {
                    if (!pc) return false;
                    const auto crypto = pc->GetConfiguration().crypto_options;
                    return crypto.sframe.require_frame_encryption;
                };
                const bool publisher_guard = required(publisher), subscriber_guard = required(subscriber);
                evidence_->Emit("native_channel_guard", {{"publisher_required", publisher_guard},
                    {"subscriber_required", subscriber_guard}});
                if (Env("E2EE_REQUIRE_NATIVE_GUARD") == "1" && (enabled_ || guard_only) && (!publisher_guard || !subscriber_guard))
                    throw std::runtime_error("native_channel_guard_missing");
            }
            const auto join = room_->join_response();
            if (join) { const auto& trailer = join->sif_trailer(); keys_->SetSifTrailer({trailer.begin(), trailer.end()}); }
            RegisterRpcHandler();
            evidence_->Emit("connected");
            video_ = std::make_shared<livekit::VideoSource>(640, 360);
            audio_ = std::make_shared<livekit::AudioSource>(48000, 1);
            livekit::VideoPublishOptions videoOptions; videoOptions.simulcast = false;
            videoOptions.auto_backup_codec = false; videoOptions.video_codec = Env("E2EE_CODEC", "vp8");
            livekit::AudioPublishPolicy audioOptions; audioOptions.red = false; audioOptions.dtx = false;
            auto videoTrack = livekit::LocalVideoTrack::createLocalVideoTrack("e0-video", video_, livekit::TrackSource::Camera, videoOptions);
            auto audioTrack = livekit::LocalAudioTrack::createLocalAudioTrack("e0-audio", audio_, audioOptions);
            co_await Publish(videoTrack, &videoOptions, nullptr);
            co_await Publish(audioTrack, nullptr, &audioOptions);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::stoi(Env("E2EE_DURATION", "60")));
            uint64_t tick = 0;
            bool reconnect_requested = false;
            while (std::chrono::steady_clock::now() < deadline) {
                if (!reconnect_requested && (Env("E2EE_RECONNECT") == "signal" || Env("E2EE_RECONNECT") == "native-full") &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()
                        >= std::stoll(Env("E2EE_RECONNECT_AT_MS"))) {
                    reconnect_requested = true;
                    const bool full = Env("E2EE_RECONNECT") == "native-full";
                    evidence_->Emit("reconnect_requested", {{"kind", full ? "native_full" : "signal"}});
                    co_await room_->SimulateScenarioAsync(full ? livekit::SimulateScenarioType::FullReconnect : livekit::SimulateScenarioType::SignalReconnect);
                }
                MaybeChangeKey();
                auto pcm = livekit::AudioFrame::create(48000, 1, 480);
                for (size_t i = 0; i < pcm.data().size(); ++i) pcm.data()[i] = static_cast<int16_t>(5000 * std::sin((tick * 480 + i) * 6.283185307179586 * 440 / 48000));
                audio_->captureFrame(pcm);
                if (tick % 10 == 0) {
                    auto frame = livekit::VideoFrame::create(640, 360, livekit::VideoBufferType::I420);
                    std::fill(frame.data(), frame.data() + 640 * 360, static_cast<uint8_t>(32 + (tick / 10) % 180));
                    std::fill(frame.data() + 640 * 360, frame.data() + frame.dataSize(), 128);
                    if (verify_relay_marker_)
                        TEST_CHECK(livekit::telemetry::EmbedE2eMediaMarker(frame,
                            {kRelayMarkerProbe, static_cast<uint32_t>(tick / 10 + 1)}));
                    livekit::VideoCaptureOptions capture;
                    capture.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    video_->captureFrame(frame, capture);
                }
                if (tick++ % 200 == 0) {
                    Sample(); SendData(); MaybeRpc();
                    if (guard_only) {
                        const auto report = co_await room_->GetStats();
                        for (const auto& pc : report.reports) for (const auto& stream : pc.inbound_rtp) {
                            evidence_->Emit("guard_inbound_rtp", {{"kind", stream.kind},
                                {"packets", stream.packets_received}, {"packets_available", stream.packets_received_available},
                                {"frames", stream.frames_decoded}, {"frames_available", stream.frames_decoded_available},
                                {"samples", stream.total_samples_received}, {"samples_available", stream.total_samples_received_available},
                                {"concealed", stream.concealed_samples}, {"concealed_available", stream.concealed_samples_available}});
                        }
                    }
                }
                timer_.expires_after(10ms); co_await timer_.async_wait(asio::use_awaitable);
            }
        } catch (...) { evidence_->Emit("operation_failed", {{"code", "fixture_operation_failed"}}); }
        closing_ = true;
        // Stop producers, disconnect PCs, then unregister observers and release crypto state.
        try { co_await room_->DisconnectAsync(); } catch (...) { evidence_->Emit("close_failed"); }
        Sample(); video_subs_.clear();
        for (auto& binding : bindings_) binding.cryptor->UnRegisterFrameCryptorTransformerObserver();
        bindings_.clear(); keys_ = nullptr;
        room_->RemoveListener(shared_from_this()); finished = true;
    }
    asio::strand<asio::io_context::executor_type> strand_; asio::steady_timer timer_;
    std::shared_ptr<Evidence> evidence_; std::shared_ptr<livekit::Room> room_;
    bool enabled_ = Env("E2EE_MODE") != "off", closing_ = false;
    bool receive_key_reinstalled_ = false;
    webrtc::scoped_refptr<webrtc::DefaultKeyProviderImpl> keys_;
    std::shared_ptr<livekit::KeyProvider> packet_keys_;
    std::vector<Binding> bindings_;
    std::vector<livekit::Track::I420VideoFrameSubscription> video_subs_;
    std::vector<std::shared_ptr<livekit::TextStreamReader>> readers_;
    std::shared_ptr<livekit::VideoSource> video_; std::shared_ptr<livekit::AudioSource> audio_;
    std::atomic<uint64_t> decoded_video_{0}, decoded_audio_samples_{0};
    static constexpr uint64_t kRelayMarkerProbe = 0x20261001;
    const bool verify_relay_marker_ = Env("E2EE_VIDEO_SOURCE") == "relay";
    std::atomic<uint64_t> valid_markers_{0}, invalid_markers_{0}, marker_advances_{0};
    std::atomic<uint32_t> last_marker_sequence_{0};
    std::atomic<bool> first_marker_correct_{false};
    std::atomic<int> decoded_width_{0}, decoded_height_{0};
};
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--business-epoch-gate") {
        livekit::RoomE2eeInteropTestAccess::BusinessEpochGate();
        std::cout << "ROOM_BUSINESS_EPOCH_GATE PASS\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--admission-gate") {
        livekit::RoomE2eeInteropTestAccess::AdmissionGate();
        std::cout << "ROOM_E2EE_ADMISSION_GATE PASS\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--policy-gate") {
        livekit::RoomE2eeInteropTestAccess::PolicyGate();
        std::cout << "ROOM_MEDIA_POLICY_GATE PASS\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--media-state-gate") {
        livekit::RoomE2eeInteropTestAccess::MediaStateGate();
        std::cout << "ROOM_MEDIA_STATE_GATE PASS\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--metadata-gate") {
        livekit::RoomE2eeInteropTestAccess::MetadataGate();
        std::cout << "ROOM_ENCRYPTION_METADATA_GATE PASS\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--adm-lifecycle") {
        // Regression for creation on a short-lived caller, then destruction
        // after that caller exits. The ADM's actual COM owner must be worker.
        SetUnhandledExceptionFilter(CrashStack);
        auto out = std::make_shared<Evidence>(Env("E2EE_EVIDENCE_DIR") + "/adm-lifecycle.jsonl");
        for (int cycle = 0; cycle < 3; ++cycle) {
            bool initialized = false;
            std::thread caller([&] { initialized = livekit::WebRTCManager::Instance().Initialize(); });
            caller.join();
            if (!initialized) { out->Emit("lifecycle_failed"); return 1; }
            livekit::WebRTCManager::Instance().Deinitialize();
            out->Emit("lifecycle_cycle", {{"cycle", cycle + 1}});
        }
        out->Emit("lifecycle_pass", {{"cycles", 3}}); return 0;
    }
    if (Env("LIVEKIT_URL").empty() || Env("LIVEKIT_TOKEN").empty() || Env("E2EE_EVIDENCE_DIR").empty()) return 2;
    SetUnhandledExceptionFilter(CrashStack);
    bool finished = true;
    const unsigned rounds = Env("E2EE_REJOIN") == "1" ? 2 : 1;
    for (fixtureGeneration = 1; fixtureGeneration <= rounds; ++fixtureGeneration) {
        asio::io_context io; auto guard = asio::make_work_guard(io);
        auto peer = std::make_shared<Peer>(io); auto done = peer->done.get_future();
        peer->Start(); std::thread worker([&] { io.run(); });
        done.wait();
        // Drain callback leases outside the strand while the I/O worker is alive.
        peer->Retire(); guard.reset(); worker.join();
        finished = finished && peer->finished;
        peer.reset();
        if (fixtureGeneration < rounds) std::this_thread::sleep_for(2s);
    }
    livekit::WebRTCManager::Instance().Deinitialize();
    return finished ? 0 : 1; // Completion only; evaluator determines interoperability verdict.
}
