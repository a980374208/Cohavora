#include "api/crypto/frame_crypto_transformer.h"
#include "api/make_ref_counted.h"
#include "rtc_base/thread.h"
#include "tests/support/test_check.h"
#include <iostream>
#include <chrono>
#include <future>
#include <thread>

class SnapshotBarrier : public webrtc::ParticipantKeyHandler {
public:
    explicit SnapshotBarrier(webrtc::KeyProvider* provider) : ParticipantKeyHandler(provider) {}
    webrtc::scoped_refptr<KeySet> GetKeySet(int index) override {
        auto snapshot = ParticipantKeyHandler::GetKeySet(index);
        if (++reads == captureOnRead) {
            captured.set_value();
            resume.get_future().wait();
        }
        return snapshot;
    }
    int reads = 0;
    int captureOnRead = 2;
    std::promise<void> captured, resume;
};

class BarrierProvider : public webrtc::DefaultKeyProviderImpl {
public:
    explicit BarrierProvider(webrtc::KeyProviderOptions& options)
        : DefaultKeyProviderImpl(options), handler(webrtc::make_ref_counted<SnapshotBarrier>(this)) {}
    const webrtc::scoped_refptr<webrtc::ParticipantKeyHandler> GetSharedKey(const std::string) override {
        return handler;
    }
    webrtc::scoped_refptr<SnapshotBarrier> handler;
};


namespace webrtc {
class MockTransformableAudioFrame : public TransformableAudioFrameInterface {
public:
    MockTransformableAudioFrame(std::vector<uint8_t> bytes, Direction direction)
        : TransformableAudioFrameInterface(Passkey{}), bytes_(std::move(bytes)), direction_(direction) {}
    ArrayView<const uint8_t> GetData() const override { return bytes_; }
    void SetData(ArrayView<const uint8_t> data) override { bytes_.assign(data.begin(), data.end()); }
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

class FrameSink : public webrtc::TransformedFrameCallback {
public:
    std::promise<std::vector<uint8_t>> first;
    unsigned count = 0;
    void OnTransformedFrame(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
        if (++count == 1) first.set_value({frame->GetData().begin(), frame->GetData().end()});
    }
};

void ConcurrentAudioInstall(bool ratchetMatches) {
    using namespace std::chrono_literals;
    auto signaling = webrtc::Thread::Create();
    TEST_CHECK(signaling->Start());
    {
        webrtc::KeyProviderOptions options;
        options.shared_key = true;
        options.ratchet_window_size = 16;
        options.failure_tolerance = -1;
        options.ratchet_salt = {'L', 'K'};
        auto tx = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(options);
        auto rx = webrtc::make_ref_counted<BarrierProvider>(options);
        rx->handler->captureOnRead = 1;
        TEST_CHECK(tx->SetSharedKey(0, {1, 2, 3}));
        rx->handler->SetKey({1, 2, 3}, 0);
        if (ratchetMatches) TEST_CHECK(!tx->RatchetSharedKey(0).empty());
        else TEST_CHECK(tx->SetSharedKey(0, {9, 8, 7}));
        auto make = [&](webrtc::scoped_refptr<webrtc::KeyProvider> provider) {
            auto cryptor = webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>(new webrtc::FrameCryptorTransformer(signaling.get(), "sender",
                webrtc::FrameCryptorTransformer::MediaType::kAudioFrame,
                webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, provider));
            cryptor->SetEnabled(true);
            cryptor->SetKeyIndex(0);
            return webrtc::scoped_refptr<webrtc::FrameTransformerInterface>(cryptor);
        };
        auto encryptor = make(tx), decryptor = make(rx);
        auto encrypt = [&](const std::vector<uint8_t>& bytes) {
            auto sink = webrtc::make_ref_counted<FrameSink>();
            auto result = sink->first.get_future();
            encryptor->RegisterTransformedFrameCallback(sink);
            encryptor->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(bytes,
                webrtc::TransformableFrameInterface::Direction::kSender));
            TEST_CHECK(result.wait_for(10s) == std::future_status::ready);
            auto sealed = result.get();
            encryptor->UnregisterTransformedFrameCallback();
            return sealed;
        };
        auto sealed = encrypt({0xf8, 0xff, 0xfe});
        auto sink = webrtc::make_ref_counted<FrameSink>();
        auto decoded = sink->first.get_future();
        decryptor->RegisterTransformedFrameCallback(sink);
        auto captured = rx->handler->captured.get_future();
        decryptor->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sealed,
            webrtc::TransformableFrameInterface::Direction::kReceiver));
        TEST_CHECK(captured.wait_for(10s) == std::future_status::ready);
        rx->handler->SetKey({4, 5, 6}, 0);
        auto installed = rx->handler->ParticipantKeyHandler::GetKeySet(0);
        TEST_CHECK(tx->SetSharedKey(0, {4, 5, 6}));
        const std::vector<uint8_t> sentinel{0xf8, 0x12, 0x34};
        auto sentinelSealed = encrypt(sentinel);
        decryptor->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sentinelSealed,
            webrtc::TransformableFrameInterface::Direction::kReceiver));
        for (unsigned n = 0; n < 100; ++n) {
            decryptor->Transform(std::make_unique<webrtc::MockTransformableAudioFrame>(sealed,
                webrtc::TransformableFrameInterface::Direction::kReceiver));
        }
        TEST_CHECK(static_cast<webrtc::FrameCryptorTransformer*>(decryptor.get())->DroppedReceiverFrames() == 100);
        rx->handler->resume.set_value();
        TEST_CHECK(decoded.wait_for(10s) == std::future_status::ready);
        TEST_CHECK(decoded.get() == sentinel);
        TEST_CHECK(rx->handler->ParticipantKeyHandler::GetKeySet(0) == installed);
        decryptor->UnregisterTransformedFrameCallback();
    }
    signaling->Stop();
}


void ConcurrentPacketInstall(bool ratchetMatches) {
    webrtc::KeyProviderOptions options;
    options.shared_key = true;
    options.ratchet_window_size = 16;
    options.ratchet_salt = {'L', 'K'};
    auto tx = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(options);
    auto rx = webrtc::make_ref_counted<BarrierProvider>(options);
    TEST_CHECK(tx->SetSharedKey(0, {1, 2, 3}));
    rx->handler->SetKey({1, 2, 3}, 0);
    if (ratchetMatches) TEST_CHECK(!tx->RatchetSharedKey(0).empty());
    else TEST_CHECK(tx->SetSharedKey(0, {9, 8, 7}));
    const auto algorithm = webrtc::FrameCryptorTransformer::Algorithm::kAesGcm;
    auto enc = webrtc::make_ref_counted<webrtc::DataPacketCryptor>(algorithm, tx);
    auto dec = webrtc::make_ref_counted<webrtc::DataPacketCryptor>(algorithm, rx);
    auto encrypted = enc->Encrypt("sender", 0, {0, 1, 2});
    TEST_CHECK(encrypted.ok());
    auto captured = rx->handler->captured.get_future();
    bool accepted = true;
    std::thread decrypt([&] { accepted = dec->Decrypt("sender", encrypted.value()).ok(); });
    captured.wait();
    // Force an install after the decrypt snapshot, before the speculative search.
    rx->handler->SetKey({4, 5, 6}, 0);
    auto installed = rx->handler->ParticipantKeyHandler::GetKeySet(0);
    rx->handler->resume.set_value();
    decrypt.join();
    TEST_CHECK(!accepted);
    TEST_CHECK(rx->handler->ParticipantKeyHandler::GetKeySet(0) == installed);
    TEST_CHECK(rx->handler->HasValidKey());
}

void PacketTransitions() {
    webrtc::KeyProviderOptions options;
    options.shared_key = true;
    options.ratchet_window_size = 16;
    options.failure_tolerance = -1;
    options.ratchet_salt = {'L', 'K'};
    auto tx = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(options);
    auto rx = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(options);
    const auto algorithm = webrtc::FrameCryptorTransformer::Algorithm::kAesGcm;
    auto encryptor = webrtc::make_ref_counted<webrtc::DataPacketCryptor>(algorithm, tx);
    auto decryptor = webrtc::make_ref_counted<webrtc::DataPacketCryptor>(algorithm, rx);
    const std::vector<uint8_t> payload{0, 1, 2, 3, 0, 255};
    auto packet = [&] {
        auto result = encryptor->Encrypt("sender", 0, payload);
        TEST_CHECK(result.ok());
        return result.value();
    };
    auto check = [&] {
        auto result = decryptor->Decrypt("sender", packet());
        TEST_CHECK(result.ok() && result.value() == payload);
    };
    TEST_CHECK(tx->SetSharedKey(0, {1, 2, 3}));
    TEST_CHECK(rx->SetSharedKey(0, {1, 2, 3}));
    check();
    for (uint8_t generation = 1; generation <= 4; ++generation) {
        auto old = packet();
        const std::vector<uint8_t> next{8, 9, generation};
        TEST_CHECK(rx->SetSharedKey(0, next));
        auto installed = rx->GetSharedKey("sender")->GetKeySet(0);
        TEST_CHECK(!decryptor->Decrypt("sender", old).ok());
        TEST_CHECK(rx->GetSharedKey("sender")->GetKeySet(0) == installed);
        TEST_CHECK(tx->SetSharedKey(0, next));
        check();
    }
    // Advance exactly to the last candidate in the default search window.
    for (int n = 0; n < 16; ++n) TEST_CHECK(!tx->RatchetSharedKey(0).empty());
    check();
    const auto accepted = rx->GetSharedKey("sender")->GetKeySet(0);
    TEST_CHECK(accepted->material == tx->ExportSharedKey(0));
    check();
    TEST_CHECK(rx->GetSharedKey("sender")->GetKeySet(0) == accepted);
    for (int n = 0; n < 3; ++n) {
        TEST_CHECK(!tx->RatchetSharedKey(0).empty());
        check();
    }
    TEST_CHECK(rx->SetSharedKey(0, {99, 98}));
    TEST_CHECK(!decryptor->Decrypt("sender", packet()).ok());
    TEST_CHECK(rx->SetSharedKey(0, tx->ExportSharedKey(0)));
    check();
}

int main() {
    ConcurrentAudioInstall(false);
    ConcurrentAudioInstall(true);
    ConcurrentPacketInstall(false);
    ConcurrentPacketInstall(true);
    PacketTransitions();
    webrtc::KeyProviderOptions options;
    options.shared_key = true;
    options.ratchet_window_size = 16;
    options.failure_tolerance = 0;
    options.discard_frame_when_cryptor_not_ready = true;
    options.ratchet_salt = {'L', 'K'};
    auto provider = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(options);
    TEST_CHECK(provider->options().discard_frame_when_cryptor_not_ready);
    TEST_CHECK(provider->SetSharedKey(0, {1, 2, 3}));
    auto handler = provider->GetSharedKey("test-participant");
    auto old = handler->GetKeySet(0);
    auto candidate = handler->DeriveKeys({7, 8, 9}, options.ratchet_salt, 128);
    TEST_CHECK(provider->SetSharedKey(0, {4, 5, 6}));
    const auto installed = handler->GetKeySet(0);
    TEST_CHECK(!handler->CommitRatchetedKey(old, candidate, 0));
    TEST_CHECK(handler->GetKeySet(0) == installed);
    TEST_CHECK(!handler->DecryptionFailureForKey(old, 0));
    TEST_CHECK(handler->HasValidKey());
    // Installing identical bytes still creates a new authoritative snapshot.
    TEST_CHECK(provider->SetSharedKey(0, {4, 5, 6}));
    TEST_CHECK(!handler->IsCurrentKey(installed, 0));
    TEST_CHECK(!handler->CommitRatchetedKey(installed, candidate, 0));
    auto current = handler->GetKeySet(0);
    TEST_CHECK(handler->CommitRatchetedKey(current, candidate, 0));
    TEST_CHECK(handler->GetKeySet(0) == candidate);
    TEST_CHECK(handler->GetKeySet(1) == nullptr);
    TEST_CHECK(handler->GetKeySet(255) == nullptr);
    TEST_CHECK(handler->DecryptionFailureForKey(candidate, 0));
    TEST_CHECK(!handler->HasValidKey());
    TEST_CHECK(provider->SetSharedKey(0, {4, 5, 6}));
    TEST_CHECK(handler->HasValidKey());
    TEST_CHECK(!handler->DecryptionFailureForKey(candidate, 0));
    std::cout << "KEY_EPOCH_GATE PASS: stale commit/failure rejected, same-material ABA rejected, atomic install recovery\n";
}
