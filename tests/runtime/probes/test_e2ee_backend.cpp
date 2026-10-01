// E0 backend ABI/encoded-frame check. Not a LiveKit Room/Flutter/media verdict.
#include "api/crypto/frame_crypto_transformer.h"
#include "api/make_ref_counted.h"
#include "rtc_base/thread.h"
#include "tests/support/test_check.h"
#include "e2ee/frame_cryptor.h"
#include "e2ee/meeting_encryption.h"
#include <thread>
#include "core/track.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <vector>
#include <set>
#include "e2ee_backend_patch/cohavora_iv_sequence.h"

namespace webrtc {
// The SDK intentionally grants this test class the frame construction passkey.
class MockTransformableAudioFrame final : public TransformableAudioFrameInterface {
public:
    explicit MockTransformableAudioFrame(std::vector<uint8_t> bytes, Direction direction)
        : TransformableAudioFrameInterface(Passkey{}), bytes_(std::move(bytes)), direction_(direction) {}
    ArrayView<const uint8_t> GetData() const override { return bytes_; }
    void SetData(ArrayView<const uint8_t> bytes) override { bytes_.assign(bytes.begin(), bytes.end()); }
    uint8_t GetPayloadType() const override { return 111; }
    uint32_t GetSsrc() const override { return 100; }
    uint32_t GetTimestamp() const override { return 48000; }
    void SetRTPTimestamp(uint32_t) override {}
    Direction GetDirection() const override { return direction_; }
    std::string GetMimeType() const override { return "audio/opus"; }
    std::optional<Timestamp> ReceiveTime() const override { return {}; }
    std::optional<Timestamp> CaptureTime() const override { return {}; }
    std::optional<TimeDelta> SenderCaptureTimeOffset() const override { return {}; }
    ArrayView<const uint32_t> GetContributingSources() const override { return {}; }
    const std::optional<uint16_t> SequenceNumber() const override { return 1; }
    std::optional<uint64_t> AbsoluteCaptureTimestamp() const override { return {}; }
    std::optional<uint8_t> AudioLevel() const override { return {}; }
private:
    std::vector<uint8_t> bytes_;
    Direction direction_;
};
}
namespace {
using namespace std::chrono_literals;
struct Sink : webrtc::TransformedFrameCallback {
    std::promise<std::vector<uint8_t>> output;
    std::atomic<unsigned> count{0};
    void OnTransformedFrame(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
        if (count.fetch_add(1) == 0) {
            const auto bytes = frame->GetData();
            output.set_value({bytes.begin(), bytes.end()});
        }
    }
};
struct Observer : webrtc::FrameCryptorTransformerObserver {
    std::atomic<unsigned> failures{0};
    std::atomic<unsigned> successes{0};
    std::promise<void> failureObserved;
    void OnFrameCryptionStateChanged(const std::string, webrtc::FrameCryptionState state) override {
        if (state == webrtc::FrameCryptionState::kOk) ++successes;
        if (state == webrtc::FrameCryptionState::kDecryptionFailed ||
            state == webrtc::FrameCryptionState::kMissingKey) {
            if (failures.fetch_add(1) == 0) failureObserved.set_value();
        }
    }
};
auto Provider(bool wrong = false, int failureTolerance = -1) {
    webrtc::KeyProviderOptions options;
    options.shared_key = true;
    const std::string salt = "LKFrameEncryptionKey";
    options.ratchet_salt.assign(salt.begin(), salt.end());
    options.ratchet_window_size = 16;
    options.failure_tolerance = failureTolerance;
    options.key_ring_size = 16;
    auto provider = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(options);
    // This package's options copy constructor omits the discard member. Assign
    // the stored option explicitly before any frame can reach the backend.
    provider->options().discard_frame_when_cryptor_not_ready = true;
    const std::string material = wrong ? "public-wrong-e0-test-material" : "public-correct-e0-test-material";
    TEST_CHECK(provider->SetSharedKey(0, {material.begin(), material.end()}));
    return provider;
}
}
int ProductTrackIsolation() {
    auto signaling = webrtc::Thread::Create(); TEST_CHECK(signaling->Start());
    {
        livekit::KeyProviderOptions options; options.shared_key = true;
        auto keys = std::make_shared<livekit::KeyProvider>(options);
        const std::string material = "public-correct-e0-test-material";
        keys->SetSharedKey({material.begin(), material.end()});
        livekit::E2eeOptions config; config.key_provider = keys;
        livekit::E2eeManager manager(config); manager.PrepareMediaSession({});
        auto missing = std::make_shared<std::promise<void>>(); auto missingFuture = missing->get_future();
        auto once = std::make_shared<std::atomic<bool>>(false);
        manager.SetMediaStateChangedHandler([missing, once](auto binding, auto state) {
            if (state == livekit::EncryptionState::MISSING_KEY && !once->exchange(true)) {
                TEST_CHECK(binding->track == "missing-slot-track"); missing->set_value();
            }
        });
        auto active = std::make_shared<std::atomic<bool>>(true);
        std::function<void()> retireGood, retireBad;
        auto good = manager.CreateReceiverCryptor(signaling.get(), "same-peer", "good-track", false, 19, active, &retireGood);
        auto bad = manager.CreateReceiverCryptor(signaling.get(), "same-peer", "missing-slot-track", false, 19, active, &retireBad);
        auto goodSink = webrtc::make_ref_counted<Sink>(), badSink = webrtc::make_ref_counted<Sink>();
        auto decoded = goodSink->output.get_future();
        good->RegisterTransformedFrameCallback(goodSink); bad->RegisterTransformedFrameCallback(badSink);
        auto sealed = [&](int slot) {
            auto provider = Provider();
            if (slot) TEST_CHECK(provider->SetSharedKey(slot, {material.begin(), material.end()}));
            auto cryptor = webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>(new webrtc::FrameCryptorTransformer(
                signaling.get(), "same-peer", webrtc::FrameCryptorTransformer::MediaType::kAudioFrame,
                webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, provider));
            cryptor->SetEnabled(true); cryptor->SetKeyIndex(slot);
            webrtc::scoped_refptr<webrtc::FrameTransformerInterface> tx = cryptor;
            auto sink = webrtc::make_ref_counted<Sink>(); auto output = sink->output.get_future();
            tx->RegisterTransformedFrameCallback(sink);
            tx->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(std::vector<uint8_t>{0xf8, 0xff, 0xfe},
                webrtc::TransformableFrameInterface::Direction::kSender));
            TEST_CHECK(output.wait_for(5s) == std::future_status::ready);
            auto bytes = output.get(); tx->UnregisterTransformedFrameCallback(); return bytes;
        };
        bad->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sealed(1),
            webrtc::TransformableFrameInterface::Direction::kReceiver));
        TEST_CHECK(missingFuture.wait_for(5s) == std::future_status::ready);
        good->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sealed(0),
            webrtc::TransformableFrameInterface::Direction::kReceiver));
        TEST_CHECK(decoded.wait_for(5s) == std::future_status::ready);
        TEST_CHECK(decoded.get() == std::vector<uint8_t>({0xf8, 0xff, 0xfe}));
        TEST_CHECK(badSink->count == 0);
        const auto status = manager.ReadMediaStatus(19); TEST_CHECK(status.tracks.size() == 2);
        TEST_CHECK(status.tracks[0].binding_id != status.tracks[1].binding_id);
        for (const auto& track : status.tracks) {
            TEST_CHECK(track.participant_identity == "same-peer");
            if (track.track_id == "good-track") TEST_CHECK(track.protected_after_current_install);
            else { TEST_CHECK(track.track_id == "missing-slot-track");
                TEST_CHECK(!track.protected_after_current_install && track.report == livekit::MediaCryptorReport::MissingKey); }
        }
        retireGood(); retireBad(); good->UnregisterTransformedFrameCallback(); bad->UnregisterTransformedFrameCallback();
        TEST_CHECK(!manager.HasActiveMediaBindings());
    }
    signaling->Stop(); std::cout << "PRODUCT_TRACK_ISOLATION PASS\n"; return 0;
}

int ProductSenderGate() {
    auto signaling = webrtc::Thread::Create();
    TEST_CHECK(signaling->Start());
    {
        livekit::KeyProviderOptions options; options.shared_key = true;
        auto keys = std::make_shared<livekit::KeyProvider>(options);
        livekit::E2eeOptions encryption; encryption.key_provider = keys;
        livekit::E2eeManager manager(encryption);
        {
            auto policy = manager.AcquireMediaPublishPolicy();
            bool blocked = false;
            try { manager.SetEnabled(false); } catch (const std::logic_error&) { blocked = true; }
            TEST_CHECK(blocked && manager.enabled());
        }
        manager.SetEnabled(false);
        {
            auto policy = manager.AcquireMediaPublishPolicy();
            bool blocked = false;
            try { manager.SetEnabled(true); } catch (const std::logic_error&) { blocked = true; }
            TEST_CHECK(blocked && !manager.enabled());
        }
        manager.SetEnabled(true);
        TEST_CHECK(!manager.CanPublishMedia("e0-probe", "vp8"));
        TEST_CHECK(!manager.CreateSenderCryptor(signaling.get(), "e0-probe", "audio", "opus", 7));
        const std::string material = "public-correct-e0-test-material";
        keys->SetSharedKey({material.begin(), material.end()});
        TEST_CHECK(manager.CanPublishMedia("e0-probe", "h264"));
        TEST_CHECK(!manager.CanPublishMedia("e0-probe", "av1"));
        auto sender = manager.CreateSenderCryptor(signaling.get(), "e0-probe", "audio", "opus", 7);
        TEST_CHECK(sender);
        auto out = webrtc::make_ref_counted<Sink>();
        auto encrypted = out->output.get_future();
        sender->RegisterTransformedFrameCallback(out);
        const std::vector<uint8_t> original{0xf8, 0xff, 0xfe};
        sender->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(original,
            webrtc::TransformableFrameInterface::Direction::kSender));
        TEST_CHECK(encrypted.wait_for(5s) == std::future_status::ready);
        auto sealed = encrypted.get();
        TEST_CHECK(sealed.size() > original.size());
        auto receiver = webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>(new webrtc::FrameCryptorTransformer(
            signaling.get(), "e0-probe", webrtc::FrameCryptorTransformer::MediaType::kAudioFrame,
            webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, Provider()));
        receiver->SetEnabled(true); receiver->SetKeyIndex(0);
        webrtc::scoped_refptr<webrtc::FrameTransformerInterface> receiverApi = receiver;
        auto decoded = webrtc::make_ref_counted<Sink>();
        auto plain = decoded->output.get_future();
        receiverApi->RegisterTransformedFrameCallback(decoded);
        receiverApi->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sealed,
            webrtc::TransformableFrameInterface::Direction::kReceiver));
        TEST_CHECK(plain.wait_for(5s) == std::future_status::ready);
        TEST_CHECK(plain.get() == original);
        bool rejected = false;
        try { manager.SetEnabled(false); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected && manager.enabled());
        TEST_CHECK(manager.media_observations().size() == 1);
        TEST_CHECK(manager.media_observations().front()->generation == 7);
        TEST_CHECK(manager.media_observations().front()->output == 1);
        TEST_CHECK(manager.ReadMediaStatus(7).tracks.front().protected_after_current_install);
        const auto expected_ratchet = Provider()->RatchetSharedKey(0);
        TEST_CHECK(keys->RatchetSharedKey(0) == expected_ratchet);
        TEST_CHECK(keys->GetSharedKey(0) == expected_ratchet);
        TEST_CHECK(keys->GetSharedKey(1).empty());
        TEST_CHECK(keys->MediaBackend()->ExportSharedKey(0) == expected_ratchet);
        keys->SetSharedKey({1, 2, 3, 4});
        TEST_CHECK(keys->MediaBackend()->ExportSharedKey(0) == std::vector<uint8_t>({1, 2, 3, 4}));
        bool empty_rejected = false;
        try { keys->SetSharedKey({}); } catch (const std::invalid_argument&) { empty_rejected = true; }
        TEST_CHECK(empty_rejected);
        TEST_CHECK(keys->GetSharedKey(0) == std::vector<uint8_t>({1, 2, 3, 4}));
        TEST_CHECK(keys->MediaBackend()->ExportSharedKey(0) == keys->GetSharedKey(0));
        livekit::KeyProvider participant_keys;
        TEST_CHECK(participant_keys.SetKey("participant", 0, {1, 2, 3, 4}));
        auto participant_backend = participant_keys.MediaBackend();
        TEST_CHECK(!participant_keys.SetKey("participant", 0, {}));
        TEST_CHECK(participant_keys.GetKey("participant", 0) == std::vector<uint8_t>({1, 2, 3, 4}));
        TEST_CHECK(participant_backend->ExportKey("participant", 0) == participant_keys.GetKey("participant", 0));
        sender->UnregisterTransformedFrameCallback();
        receiverApi->UnregisterTransformedFrameCallback();
    }
    signaling->Stop();
    std::cout << "PRODUCT_SENDER_GATE PASS: missing-key admission, codec rejection, actual backend encryption, no active downgrade, key synchronization\n";
    return 0;
}
int ProductReceiverGate() {
    livekit::TrackPublication publication(nullptr, "remote", "audio");
    TEST_CHECK(publication.encryption() == livekit::TrackEncryption::Unknown);
    publication.set_encryption(livekit::TrackEncryption::Gcm);
    livekit::TrackPublication copied(publication);
    TEST_CHECK(copied.SnapshotState().encryption == livekit::TrackEncryption::Gcm);
    publication.set_encryption(livekit::TrackEncryption::None);
    copied = publication;
    TEST_CHECK(copied.SnapshotState().encryption == livekit::TrackEncryption::None);
    auto signaling = webrtc::Thread::Create();
    TEST_CHECK(signaling->Start());
    {
        livekit::KeyProviderOptions options; options.shared_key = true;
        auto keys = std::make_shared<livekit::KeyProvider>(options);
        livekit::E2eeOptions encryption; encryption.key_provider = keys;
        livekit::E2eeManager manager(encryption);
        auto state_seen = std::make_shared<std::promise<void>>();
        auto state_future = state_seen->get_future();
        auto notified = std::make_shared<std::atomic<bool>>(false);
        manager.SetMediaStateChangedHandler([state_seen, notified](
            std::shared_ptr<const livekit::E2eeManager::MediaObservation> binding,
            livekit::EncryptionState state) {
            if (state != livekit::EncryptionState::OK || notified->exchange(true)) return;
            TEST_CHECK(binding->identity == "e0-probe" && binding->track == "remote-audio");
            TEST_CHECK(binding->receiving && binding->generation == 7);
            state_seen->set_value();
        });
        const std::vector<uint8_t> trailer{0x53, 0x49, 0x46, 0x01};
        manager.PrepareMediaSession(trailer);
        auto original_backend = keys->MediaBackend();
        TEST_CHECK(original_backend->options().uncrypted_magic_bytes == trailer);
        auto active = std::make_shared<std::atomic<bool>>(false);
        std::function<void()> retire;
        auto rx = manager.CreateReceiverCryptor(signaling.get(), "e0-probe", "remote-audio", false, 7, active, &retire);
        TEST_CHECK(rx);
        TEST_CHECK(!manager.CreateReceiverCryptor(signaling.get(), "", "remote-audio", false, 7, active));
        auto observations = manager.media_observations();
        TEST_CHECK(observations.size() == 1 && observations[0]->receiving && observations[0]->generation == 7);
        TEST_CHECK(manager.ReadMediaStatus(7).tracks.empty()); // Not committed.
        bool rejected = false;
        try { manager.SetEnabled(false); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected && manager.enabled());
        rejected = false;
        try { manager.PrepareMediaSession({}); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected);
        const std::string material = "public-correct-e0-test-material";
        keys->SetSharedKey({material.begin(), material.end()});
        auto cryptor = webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>(new webrtc::FrameCryptorTransformer(
            signaling.get(), "e0-probe", webrtc::FrameCryptorTransformer::MediaType::kAudioFrame,
            webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, Provider()));
        cryptor->SetEnabled(true);
        webrtc::scoped_refptr<webrtc::FrameTransformerInterface> tx = cryptor;
        auto enc = webrtc::make_ref_counted<Sink>();
        auto dec = webrtc::make_ref_counted<Sink>();
        auto encrypted = enc->output.get_future();
        auto decrypted = dec->output.get_future();
        tx->RegisterTransformedFrameCallback(enc);
        auto gate = livekit::CreateMediaReceiverGate();
        auto hook = gate->Hook();
        hook->RegisterTransformedFrameCallback(dec);
        const std::vector<uint8_t> original{0xf8, 0xff, 0xfe};
        tx->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(original,
            webrtc::TransformableFrameInterface::Direction::kSender));
        TEST_CHECK(encrypted.wait_for(5s) == std::future_status::ready);
        const auto sealed = encrypted.get();
        auto receive = [&] { hook->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sealed,
            webrtc::TransformableFrameInterface::Direction::kReceiver)); };
        receive();
        TEST_CHECK(observations[0]->input == 0); // No metadata: no cryptor target.
        TEST_CHECK(gate->SetTarget(7, rx));
        receive();
        TEST_CHECK(observations[0]->input == 0); // Not yet committed by Room.
        active->store(true, std::memory_order_release);
        receive();
        TEST_CHECK(decrypted.wait_for(5s) == std::future_status::ready);
        TEST_CHECK(decrypted.get() == original && observations[0]->output == 1);
        TEST_CHECK(state_future.wait_for(5s) == std::future_status::ready);
        const auto status = manager.ReadMediaStatus(7);
        TEST_CHECK(status.enabled && status.tracks.size() == 1);
        TEST_CHECK(status.tracks[0].binding_id != 0 && status.tracks[0].receiving);
        TEST_CHECK(status.tracks[0].report == livekit::MediaCryptorReport::Ok);
        TEST_CHECK(status.tracks[0].protected_after_current_install);
        TEST_CHECK(manager.ReadMediaStatus(8).tracks.empty());
        TEST_CHECK(manager.InstallRecoveryKey(livekit::MeetingSecretHandle::Create({material.begin(), material.end()})));
        TEST_CHECK(!manager.ReadMediaStatus(7).tracks[0].protected_after_current_install);
        auto sif_sink = webrtc::make_ref_counted<Sink>();
        auto sif_output = sif_sink->output.get_future();
        hook->RegisterTransformedFrameCallback(sif_sink);
        auto sif_frame = original;
        sif_frame.insert(sif_frame.end(), trailer.begin(), trailer.end());
        hook->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sif_frame,
            webrtc::TransformableFrameInterface::Direction::kReceiver));
        TEST_CHECK(sif_output.wait_for(5s) == std::future_status::ready);
        TEST_CHECK(sif_output.get() == original && observations[0]->output == 2);
        TEST_CHECK(!manager.ReadMediaStatus(7).tracks[0].protected_after_current_install); // SIF is not crypto proof.
        auto recovered_sink = webrtc::make_ref_counted<Sink>();
        auto recovered_output = recovered_sink->output.get_future();
        hook->RegisterTransformedFrameCallback(recovered_sink);
        receive();
        TEST_CHECK(recovered_output.wait_for(5s) == std::future_status::ready);
        TEST_CHECK(recovered_output.get() == original);
        TEST_CHECK(manager.ReadMediaStatus(7).tracks[0].protected_after_current_install);
        active->store(false, std::memory_order_release);
        TEST_CHECK(manager.ReadMediaStatus(7).tracks.empty());
        receive();
        TEST_CHECK(observations[0]->input == 3); // Retired binding rejects entry.
        retire();
        retire(); // Exact-instance retirement is idempotent.
        TEST_CHECK(!manager.HasActiveMediaBindings());
        active->store(true, std::memory_order_release);
        receive();
        TEST_CHECK(observations[0]->retired.load() && observations[0]->input == 3);
        TEST_CHECK(manager.ReadMediaStatus(7).tracks.empty()); // Cannot revive a retired binding.
        std::function<void()> retire_replacement;
        auto replacement = manager.CreateReceiverCryptor(signaling.get(), "e0-probe", "remote-audio", false, 8, active, &retire_replacement);
        const auto replacement_status = manager.ReadMediaStatus(8);
        TEST_CHECK(replacement_status.tracks.size() == 1);
        TEST_CHECK(replacement_status.tracks[0].binding_id != status.tracks[0].binding_id);
        TEST_CHECK(replacement_status.tracks[0].report == livekit::MediaCryptorReport::Waiting);
        TEST_CHECK(!replacement_status.tracks[0].protected_after_current_install);
        auto replacement_sink = webrtc::make_ref_counted<Sink>();
        auto replacement_output = replacement_sink->output.get_future();
        hook->RegisterTransformedFrameCallback(replacement_sink);
        TEST_CHECK(gate->SetTarget(8, replacement));
        TEST_CHECK(!gate->SetTarget(7, rx));
        TEST_CHECK(!gate->SetTarget(8, nullptr));
        retire(); // An old binding's cleanup must never retire its replacement.
        hook->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sealed,
            webrtc::TransformableFrameInterface::Direction::kReceiver));
        TEST_CHECK(replacement_output.wait_for(5s) == std::future_status::ready);
        TEST_CHECK(replacement_output.get() == original);
        hook->UnregisterTransformedFrameCallback();
        retire_replacement();
        replacement = nullptr;
        TEST_CHECK(!manager.HasActiveMediaBindings());
        manager.SetEnabled(false); // Retained retired observations are not leases.
        manager.PrepareMediaSession({});
        TEST_CHECK(keys->MediaBackend()->options().uncrypted_magic_bytes.empty());
        TEST_CHECK(original_backend->options().uncrypted_magic_bytes == trailer);
        tx->UnregisterTransformedFrameCallback();
        rx->UnregisterTransformedFrameCallback();
        rx = nullptr;
        TEST_CHECK(observations[0]->retired.load());
    }
    {
        auto keys = std::make_shared<livekit::KeyProvider>();
        livekit::E2eeOptions options; options.key_provider = keys;
        auto first = std::make_unique<livekit::E2eeManager>(options);
        first->PrepareMediaSession({1, 2, 3});
        auto backend = keys->MediaBackend();
        auto active = std::make_shared<std::atomic<bool>>(false);
        std::function<void()> retire;
        auto receiver = first->CreateReceiverCryptor(signaling.get(), "owner-peer", "owner-track",
            false, 1, active, &retire);
        TEST_CHECK(receiver);
        livekit::E2eeManager second(options);
        bool rejected = false;
        try { second.PrepareMediaSession({4}); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected && keys->MediaBackend() == backend);
        first.reset(); // A surviving native binding still owns this provider.
        rejected = false;
        try { second.PrepareMediaSession({4}); } catch (const std::logic_error&) { rejected = true; }
        TEST_CHECK(rejected && backend->options().uncrypted_magic_bytes == std::vector<uint8_t>({1, 2, 3}));
        retire();
        second.PrepareMediaSession({4});
        TEST_CHECK(keys->MediaBackend()->options().uncrypted_magic_bytes == std::vector<uint8_t>({4}));
    }
    signaling->Stop();
    std::cout << "PRODUCT_RECEIVER_GATE PASS: missing-key binding, actual decryption, inactive/retired input rejection, policy lease\n";
    return 0;
}

void TestKeyProviderParameters() {
    const auto reject = [](livekit::KeyProviderOptions options) {
        bool rejected = false;
        try { livekit::KeyProvider provider(options); }
        catch (const std::invalid_argument&) { rejected = true; }
        TEST_CHECK(rejected);
    };
    for (int ring : {-1, 0, 256}) {
        livekit::KeyProviderOptions options; options.key_ring_size = ring; reject(options);
    }
    livekit::KeyProviderOptions options;
    options.ratchet_window_size = -1; reject(options);
    options = {}; options.failure_tolerance = -2; reject(options);
    options = {}; options.key_derivation_algorithm = static_cast<livekit::KeyDerivationAlgorithm>(99); reject(options);
    options = {}; options.key_ring_size = 1; options.ratchet_window_size = 0;
    livekit::KeyProvider provider(options);
    provider.SetSharedKey({1, 2, 3});
    for (int index : {-1, 1}) {
        bool rejected = false;
        try { provider.SetSharedKey({9}, index); }
        catch (const std::out_of_range&) { rejected = true; }
        TEST_CHECK(rejected);
        TEST_CHECK(!provider.SetKey("alice", index, {9}));
    }
    TEST_CHECK(provider.GetSharedKey() == std::vector<uint8_t>({1, 2, 3}));
    TEST_CHECK(!provider.SetKey("", 0, {9}));
    options.key_ring_size = 255;
    livekit::KeyProvider largest(options);
    largest.SetSharedKey({7}, 254);
    TEST_CHECK(largest.GetSharedKey(254) == std::vector<uint8_t>({7}));
}

void TestMeetingSecretHandle() {
    using namespace livekit;
    auto secret = MeetingSecretHandle::Create({'a', 'b', 'c'});
    MeetingEncryptionRequest request{MeetingEncryptionMode::Required, secret};
    request.Validate();
    std::atomic<int> installed{0}, stale{0};
    auto consume = [&] {
        try {
            auto provider = secret->ConsumeProvider();
            TEST_CHECK(provider->options().shared_key);
            TEST_CHECK(provider->GetSharedKey() == std::vector<uint8_t>({'a', 'b', 'c'}));
            ++installed;
        } catch (const EncryptionRequestException& error) {
            TEST_CHECK(error.code() == EncryptionRequestError::StaleContext);
            ++stale;
        }
    };
    std::thread first(consume), second(consume); first.join(); second.join();
    TEST_CHECK(installed == 1 && stale == 1 && !secret->available());
    auto cancelled = MeetingSecretHandle::Create({'d', 'e', 'f'});
    MeetingEncryptionRequest copy{MeetingEncryptionMode::Required, cancelled};
    auto alias = copy;
    copy.Revoke();
    TEST_CHECK(!cancelled->available());
    bool rejected = false;
    try { alias.Validate(); } catch (const EncryptionRequestException&) { rejected = true; }
    TEST_CHECK(rejected);
    for (const auto size : {0u, 31u, 33u, 4097u}) {
        rejected = false;
        try { MeetingSecretHandle::Create(std::vector<uint8_t>(size, 1), KeyMaterialFormat::Raw32); }
        catch (const EncryptionRequestException&) { rejected = true; }
        TEST_CHECK(rejected);
    }
    for (const std::vector<uint8_t> invalid : {std::vector<uint8_t>{0xc0, 0x80}, {0xed, 0xa0, 0x80}, {0xf4, 0x90, 0x80, 0x80}, {0xe4, 0xb8}, {0xe4, 0xb8, 0xad}, {0xc3, 0xa9}, {0xf0, 0x9f, 0x94, 0x92}, {0}, {0x1f}, {0x7f}}) {
        rejected = false;
        try { MeetingSecretHandle::Create(invalid); }
        catch (const EncryptionRequestException&) { rejected = true; }
        TEST_CHECK(rejected);
    }
    for (auto bytes : {std::vector<uint8_t>{0x20, 0x7e}, std::vector<uint8_t>(4096, 'A')}) {
        auto text = MeetingSecretHandle::Create(bytes);
        TEST_CHECK(text->ConsumeProvider()->GetSharedKey() == bytes);
    }
    rejected = false;
    try { MeetingSecretHandle::Create(std::vector<uint8_t>(4097, 'A')); }
    catch (const EncryptionRequestException&) { rejected = true; }
    TEST_CHECK(rejected);
    auto binary = MeetingSecretHandle::Create(std::vector<uint8_t>(32, 0xff), KeyMaterialFormat::Raw32);
    TEST_CHECK(binary->ConsumeProvider()->GetSharedKey() == std::vector<uint8_t>(32, 0xff));
    MeetingEncryptionRequest{}.Validate();
}

void TestMediaFrameEvidence() {
    livekit::MediaFrameEvidence evidence;
    std::array<int, 130> frames{};
    evidence.Admit(&frames[0], 2, true);
    TEST_CHECK(!evidence.Complete(&frames[0], 4)); // Old work finished after replacement.
    TEST_CHECK(evidence.protected_epoch() == 0);
    evidence.Admit(&frames[0], 3, true);
    TEST_CHECK(!evidence.Complete(&frames[0], 4)); // Install was in progress at input.
    evidence.Admit(&frames[0], 4, true);
    TEST_CHECK(!evidence.Complete(&frames[0], 5)); // Install began before completion.
    evidence.Admit(&frames[0], 6, true);
    evidence.Admit(&frames[0], 6, false); // Reused address now contains SIF.
    TEST_CHECK(!evidence.Complete(&frames[0], 6));
    for (auto& frame : frames) evidence.Admit(&frame, 6, true);
    TEST_CHECK(!evidence.Complete(&frames[0], 6)); // Bounded overflow is unknown.
    TEST_CHECK(evidence.Complete(&frames.back(), 6));
    TEST_CHECK(evidence.protected_epoch() == 6);
    TEST_CHECK(!evidence.Complete(&frames.back(), 6)); // One completion only.
    livekit::KeyProvider keys;
    const auto epoch = keys.MediaInstallEpoch();
    const auto initial = epoch->load();
    keys.SetSharedKey({1, 2, 3});
    TEST_CHECK(epoch->load() == initial + 2 && !(epoch->load() & 1));
    keys.RatchetSharedKey();
    TEST_CHECK(epoch->load() == initial + 4);
    keys.MediaBackend(); // Per-participant backend rejects shared installation.
    keys.SetSharedKey({4, 5, 6});
    TEST_CHECK(epoch->load() & 1); // Failed install cannot publish a verified epoch.
    keys.SetKey("participant", 0, {7, 8, 9});
    TEST_CHECK(!(epoch->load() & 1));
}

void TestIvSequence() {
    using Sequence = cohavora_e2ee::IvSequence;
    const auto seed = +[](Sequence::Iv& value) { value.fill(0xff); return true; };
    Sequence bounded(seed, 8);
    std::set<Sequence::Iv> seen;
    Sequence::Iv iv{};
    for (int i = 0; i < 8; ++i) { TEST_CHECK(bounded.Next(iv)); TEST_CHECK(seen.insert(iv).second); }
    const auto last = iv;
    TEST_CHECK(!bounded.Next(iv) && iv == last);
    Sequence failed(+[](Sequence::Iv&) { return false; });
    TEST_CHECK(!failed.Next(iv) && !failed.Next(iv) && iv == last);
    Sequence invalid(seed, Sequence::kMaximumInvocations + 1);
    TEST_CHECK(!invalid.Next(iv));
    Sequence concurrent(seed);
    std::mutex outputMutex;
    seen.clear();
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back([&] {
        for (int j = 0; j < 256; ++j) {
            Sequence::Iv value;
            TEST_CHECK(concurrent.Next(value));
            std::lock_guard lock(outputMutex);
            TEST_CHECK(seen.insert(value).second);
        }
    });
    for (auto& thread : threads) thread.join();
    TEST_CHECK(seen.size() == 1024);
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--track-isolation") return ProductTrackIsolation();
    TestIvSequence();
    TestMediaFrameEvidence();
    TestKeyProviderParameters();
    TestMeetingSecretHandle();
    if (argc == 2 && std::string(argv[1]) == "--product-sender") return ProductSenderGate();
    if (argc == 2 && std::string(argv[1]) == "--product-receiver") return ProductReceiverGate();
    auto signaling = webrtc::Thread::Create();
    TEST_CHECK(signaling->Start());
    const auto provider = Provider();
    const auto wrong = Provider(true);
    auto make = [&](webrtc::scoped_refptr<webrtc::KeyProvider> keys) {
        auto value = webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>(new webrtc::FrameCryptorTransformer(
            signaling.get(), "e0-probe", webrtc::FrameCryptorTransformer::MediaType::kAudioFrame,
            webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, keys));
        value->SetKeyIndex(0);
        value->SetEnabled(true);
        return value;
    };
    auto encryptor = make(provider);
    auto decryptor = make(Provider());
    auto rejected = make(Provider(true, 0));
    auto enc = webrtc::make_ref_counted<Sink>();
    auto dec = webrtc::make_ref_counted<Sink>();
    auto bad = webrtc::make_ref_counted<Sink>();
    auto observer = webrtc::make_ref_counted<Observer>();
    rejected->RegisterFrameCryptorTransformerObserver(observer);
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> tx = encryptor, rx = decryptor, badRx = rejected;
    tx->RegisterTransformedFrameCallback(enc);
    rx->RegisterTransformedFrameCallback(dec);
    badRx->RegisterTransformedFrameCallback(bad);
    const std::vector<uint8_t> original{0xf8, 0xff, 0xfe}; // Public Opus comfort-noise fixture.
    auto encrypted = enc->output.get_future();
    tx->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(original,
        webrtc::TransformableFrameInterface::Direction::kSender));
    TEST_CHECK(encrypted.wait_for(5s) == std::future_status::ready);
    const auto sealed = encrypted.get();
    TEST_CHECK(sealed.size() > original.size());
    auto decrypted = dec->output.get_future();
    rx->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sealed,
        webrtc::TransformableFrameInterface::Direction::kReceiver));
    TEST_CHECK(decrypted.wait_for(5s) == std::future_status::ready);
    TEST_CHECK(decrypted.get() == original);
    auto forbidden = bad->output.get_future();
    auto failureObserved = observer->failureObserved.get_future();
    badRx->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sealed,
        webrtc::TransformableFrameInterface::Direction::kReceiver));
    // The 16-slot ratchet search performs PBKDF2. Wait for the backend's
    // authenticated failure, not an arbitrary two-second quiet interval.
    TEST_CHECK(failureObserved.wait_for(30s) == std::future_status::ready);
    TEST_CHECK(forbidden.wait_for(0s) == std::future_status::timeout);
    TEST_CHECK(observer->failures.load() > 0);
    // Default -1 suppresses the failure event. A valid sentinel queued AFTER
    // the wrong-key frame proves completion and zero unauthenticated output;
    // absence of an error event is deliberately not the success criterion.
    const std::vector<uint8_t> sentinel{0xf8, 0x12, 0x34};
    auto sentinelTx = make(wrong);
    auto defaultRx = make(Provider(true));
    auto sentinelEnc = webrtc::make_ref_counted<Sink>();
    auto sentinelDec = webrtc::make_ref_counted<Sink>();
    auto defaultObserver = webrtc::make_ref_counted<Observer>();
    defaultRx->RegisterFrameCryptorTransformerObserver(defaultObserver);
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> sentinelTxApi = sentinelTx, defaultRxApi = defaultRx;
    sentinelTxApi->RegisterTransformedFrameCallback(sentinelEnc);
    defaultRxApi->RegisterTransformedFrameCallback(sentinelDec);
    auto sentinelEncrypted = sentinelEnc->output.get_future();
    sentinelTxApi->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sentinel,
        webrtc::TransformableFrameInterface::Direction::kSender));
    TEST_CHECK(sentinelEncrypted.wait_for(5s) == std::future_status::ready);
    auto sentinelDecoded = sentinelDec->output.get_future();
    defaultRxApi->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sealed,
        webrtc::TransformableFrameInterface::Direction::kReceiver));
    defaultRxApi->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sentinelEncrypted.get(),
        webrtc::TransformableFrameInterface::Direction::kReceiver));
    TEST_CHECK(sentinelDecoded.wait_for(30s) == std::future_status::ready);
    TEST_CHECK(sentinelDecoded.get() == sentinel);
    TEST_CHECK(sentinelDec->count.load() == 1);
    TEST_CHECK(defaultObserver->failures.load() == 0);
    // Stop frame producers (none remain), then unregister callbacks and release
    // cryptors while their signaling thread is alive.
    rejected->UnRegisterFrameCryptorTransformerObserver();
    tx->UnregisterTransformedFrameCallback(); rx->UnregisterTransformedFrameCallback();
    badRx->UnregisterTransformedFrameCallback();
    tx = nullptr; rx = nullptr; badRx = nullptr;
    encryptor = nullptr; decryptor = nullptr; rejected = nullptr;
    defaultRx->UnRegisterFrameCryptorTransformerObserver();
    sentinelTxApi->UnregisterTransformedFrameCallback();
    defaultRxApi->UnregisterTransformedFrameCallback();
    sentinelTxApi = nullptr; defaultRxApi = nullptr;
    sentinelTx = nullptr; defaultRx = nullptr;
    signaling->Stop();
    std::cout << "E2EE_BACKEND {\"result\":\"PASS\",\"encrypted_frames\":1,\"decrypted_frames\":1,"
                 "\"wrong_key_deliveries\":0,\"default_failure_events\":0,"
                 "\"explicit_failure_events\":1,\"scope\":\"encoded_audio_backend_only\"}\n";
}
