#include "frame_cryptor.h"
#include "meeting_encryption.h"
#include "api/crypto/frame_crypto_transformer.h"
#include <algorithm>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <iostream>
#include "api/make_ref_counted.h"

namespace livekit {

namespace {
class MediaReceiverHook : public webrtc::FrameTransformerInterface {
public:
    explicit MediaReceiverHook(std::shared_ptr<MediaReceiverGate> gate) : gate_(std::move(gate)) {}
    void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override { gate_->Transform(std::move(frame)); }
    void RegisterTransformedFrameCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) override { gate_->Register(std::move(callback)); }
    void RegisterTransformedFrameSinkCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback, uint32_t ssrc) override { gate_->Register(std::move(callback), ssrc); }
    void UnregisterTransformedFrameCallback() override { gate_->Unregister(); }
    void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override { gate_->Unregister(ssrc); }
private:
    std::shared_ptr<MediaReceiverGate> gate_;
};
class MediaDiscarder : public webrtc::FrameTransformerInterface {
public:
    void Transform(std::unique_ptr<webrtc::TransformableFrameInterface>) override {}
    void RegisterTransformedFrameCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback>) override {}
    void RegisterTransformedFrameSinkCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback>, uint32_t) override {}
    void UnregisterTransformedFrameCallback() override {}
    void UnregisterTransformedFrameSinkCallback(uint32_t) override {}
};
bool MediaActive(const E2eeManager::MediaObservation& observation) {
    return !observation.retired.load(std::memory_order_acquire) &&
        (!observation.binding_active || observation.binding_active->load(std::memory_order_acquire));
}
class MediaOutput : public webrtc::TransformedFrameCallback {
public:
    MediaOutput(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> target,
                std::shared_ptr<E2eeManager::MediaObservation> observation)
        : target_(std::move(target)), observation_(std::move(observation)) {}
    void OnTransformedFrame(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
        if (!MediaActive(*observation_)) return;
        observation_->frame_evidence.Complete(frame.get(), observation_->install_epoch->load(std::memory_order_acquire));
        observation_->output.fetch_add(1, std::memory_order_relaxed);
        target_->OnTransformedFrame(std::move(frame));
    }
private:
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> target_;
    std::shared_ptr<E2eeManager::MediaObservation> observation_;
};
class MediaState : public webrtc::FrameCryptorTransformerObserver {
public:
    MediaState(std::shared_ptr<E2eeManager::MediaObservation> observation,
               E2eeManager::MediaStateChangedHandler handler)
        : observation_(std::move(observation)), handler_(std::move(handler)) {}
    void OnFrameCryptionStateChanged(const std::string, webrtc::FrameCryptionState state) override {
        if (!MediaActive(*observation_)) return;
        observation_->state.store(static_cast<int>(state), std::memory_order_relaxed);
        EncryptionState mapped = EncryptionState::INTERNAL_ERROR;
        switch (state) {
        case webrtc::kNew: mapped = EncryptionState::NEW; break;
        case webrtc::kOk: mapped = EncryptionState::OK; break;
        case webrtc::kEncryptionFailed: mapped = EncryptionState::ENCRYPTION_FAILED; break;
        case webrtc::kDecryptionFailed: mapped = EncryptionState::DECRYPTION_FAILED; break;
        case webrtc::kMissingKey: mapped = EncryptionState::MISSING_KEY; break;
        case webrtc::kKeyRatcheted: mapped = EncryptionState::KEY_RATCHETED; break;
        case webrtc::kInternalError: break;
        }
        observation_->reported_state.store(mapped, std::memory_order_release);
        if (handler_) handler_(observation_, mapped);
    }
private:
    std::shared_ptr<E2eeManager::MediaObservation> observation_;
    E2eeManager::MediaStateChangedHandler handler_;
};
class MediaTransformer : public webrtc::FrameTransformerInterface {
public:
    MediaTransformer(webrtc::scoped_refptr<webrtc::FrameCryptorTransformer> cryptor,
                std::shared_ptr<E2eeManager::MediaObservation> observation, std::shared_ptr<std::atomic<int>> index, std::shared_ptr<void> owner)
        : cryptor_(std::move(cryptor)), observation_(std::move(observation)), index_(std::move(index)), owner_(std::move(owner)) {}
    ~MediaTransformer() override {
        Retire();
    }
    void Retire() {
        observation_->retired.store(true, std::memory_order_release);
        std::shared_ptr<void> retired_owner;
        webrtc::scoped_refptr<webrtc::FrameCryptorTransformer> retired;
        {
            std::lock_guard lock(cryptor_mutex_);
            retired.swap(cryptor_);
            retired_owner.swap(owner_);
        }
        if (retired) retired->UnRegisterFrameCryptorTransformerObserver();
    }
    void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) override {
        if (!MediaActive(*observation_)) return;
        const auto mime = frame->GetMimeType();
        unsigned bit = mime == "audio/opus" ? 0 : mime == "video/VP8" ? 1 :
            mime == "video/VP9" ? 2 : mime == "video/H264" ? 3 : 4;
        observation_->codec_mask.fetch_or(1u << bit, std::memory_order_relaxed);
        observation_->input.fetch_add(1, std::memory_order_relaxed);
        // Unsupported encoded output is never sent unprotected or passed through.
        if (bit == 4 || (observation_->receiving ? ((bit != 0) != observation_->video)
                                               : bit != observation_->expected_codec)) {
            observation_->state.store(static_cast<int>(webrtc::FrameCryptionState::kInternalError));
            observation_->reported_state.store(EncryptionState::INTERNAL_ERROR, std::memory_order_release);
            return;
        }
        auto cryptor = Cryptor();
        if (!cryptor) return;
        const auto bytes = frame->GetData();
        const auto& trailer = observation_->sif_trailer;
        const bool sif = observation_->receiving && !trailer.empty() && bytes.size() >= trailer.size() &&
            std::equal(trailer.begin(), trailer.end(), bytes.end() - trailer.size());
        observation_->frame_evidence.Admit(frame.get(),
            observation_->install_epoch->load(std::memory_order_acquire), !bytes.empty() && !sif);
        if (index_) {
            const int selected = index_->load(std::memory_order_acquire);
            if (last_index_.exchange(selected) != selected) cryptor->SetKeyIndex(selected);
        }
        static_cast<webrtc::FrameTransformerInterface*>(cryptor.get())->Transform(std::move(frame));
    }
    void RegisterTransformedFrameCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> sink) override {
        if (auto cryptor = Cryptor()) static_cast<webrtc::FrameTransformerInterface*>(cryptor.get())->RegisterTransformedFrameCallback(webrtc::make_ref_counted<MediaOutput>(sink, observation_));
    }
    void RegisterTransformedFrameSinkCallback(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> sink, uint32_t ssrc) override {
        if (auto cryptor = Cryptor()) static_cast<webrtc::FrameTransformerInterface*>(cryptor.get())->RegisterTransformedFrameSinkCallback(webrtc::make_ref_counted<MediaOutput>(sink, observation_), ssrc);
    }
    void UnregisterTransformedFrameCallback() override { if (auto cryptor = Cryptor()) static_cast<webrtc::FrameTransformerInterface*>(cryptor.get())->UnregisterTransformedFrameCallback(); }
    void UnregisterTransformedFrameSinkCallback(uint32_t ssrc) override { if (auto cryptor = Cryptor()) static_cast<webrtc::FrameTransformerInterface*>(cryptor.get())->UnregisterTransformedFrameSinkCallback(ssrc); }
private:
    webrtc::scoped_refptr<webrtc::FrameCryptorTransformer> Cryptor() {
        std::lock_guard lock(cryptor_mutex_);
        return cryptor_;
    }
    std::mutex cryptor_mutex_;
    webrtc::scoped_refptr<webrtc::FrameCryptorTransformer> cryptor_;
    std::shared_ptr<E2eeManager::MediaObservation> observation_;
    std::shared_ptr<std::atomic<int>> index_;
    std::shared_ptr<void> owner_;
    std::atomic<int> last_index_{-1};
};
}

std::shared_ptr<MediaReceiverGate> CreateMediaReceiverGate() {
    auto gate = std::make_shared<MediaReceiverGate>();
    gate->self_ = gate;
    return gate;
}
webrtc::scoped_refptr<webrtc::FrameTransformerInterface> MediaReceiverGate::Hook() {
    return webrtc::make_ref_counted<MediaReceiverHook>(self_.lock());
}
bool MediaReceiverGate::SetTarget(uint64_t binding_serial, webrtc::scoped_refptr<webrtc::FrameTransformerInterface> target) {
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> previous;
    {
        std::lock_guard lock(mutex_);
        if (binding_serial <= binding_serial_) return false;
        if (target) {
            if (callback_) target->RegisterTransformedFrameCallback(callback_);
            for (const auto& [ssrc, callback] : sinks_) target->RegisterTransformedFrameSinkCallback(callback, ssrc);
        }
        previous.swap(target_);
        target_ = std::move(target);
        binding_serial_ = binding_serial;
    }
    // Destroying an old backend may join its queue. Never do so under this lock.
    return true;
}
void MediaReceiverGate::Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame) {
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> target;
    { std::lock_guard lock(mutex_); target = target_; }
    if (target) target->Transform(std::move(frame));
}
void MediaReceiverGate::Register(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback) {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
    if (target_) target_->RegisterTransformedFrameCallback(callback_);
}
void MediaReceiverGate::Register(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback, uint32_t ssrc) {
    std::lock_guard lock(mutex_);
    sinks_[ssrc] = std::move(callback);
    if (target_) target_->RegisterTransformedFrameSinkCallback(sinks_[ssrc], ssrc);
}
void MediaReceiverGate::Unregister() {
    std::lock_guard lock(mutex_);
    if (target_) target_->UnregisterTransformedFrameCallback();
    callback_ = nullptr;
}
void MediaReceiverGate::Unregister(uint32_t ssrc) {
    std::lock_guard lock(mutex_);
    if (target_) target_->UnregisterTransformedFrameSinkCallback(ssrc);
    sinks_.erase(ssrc);
}

bool E2eeManager::CanPublishMedia(const std::string& identity, const std::string& codec) const {
    if (codec != "opus" && codec != "vp8" && codec != "h264" && codec != "vp9") return false;
    if (options_.encryption_type != EncryptionType::GCM || !options_.key_provider) return false;
    const auto state = data_packet_state();
    if (!state.enabled) return false;
    const int media_index = media_send_index_->load(std::memory_order_acquire);
    const auto key = options_.key_provider->options().shared_key
        ? options_.key_provider->GetSharedKey(media_index)
        : options_.key_provider->GetKey(identity, media_index);
    return !identity.empty() && !key.empty();
}

webrtc::scoped_refptr<webrtc::FrameTransformerInterface> E2eeManager::CreateSenderCryptor(
    webrtc::Thread* signaling, const std::string& identity, const std::string& track,
    const std::string& codec, uint64_t generation) {
    if (!signaling || !CanPublishMedia(identity, codec)) return nullptr;
    // Serialize admission and observation registration with SetEnabled: a
    // concurrent disable must not slip between the check and binding creation.
    std::lock_guard lock(mutex_);
    if (!enabled_) return nullptr;
    const bool video = codec != "opus";
    options_.key_provider->ClaimMediaOwner(media_owner_);
    auto observation = std::make_shared<MediaObservation>();
    observation->binding_id = ++next_media_binding_id_;
    observation->install_epoch = options_.key_provider->MediaInstallEpoch();
    observation->sif_trailer = options_.key_provider->MediaBackend()->options().uncrypted_magic_bytes;
    observation->track = track; observation->identity = identity; observation->video = video; observation->generation = generation;
    observation->expected_codec = codec == "opus" ? 0 : codec == "vp8" ? 1 : codec == "vp9" ? 2 : 3;
    auto cryptor = webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>(new webrtc::FrameCryptorTransformer(
        signaling, identity, video ? webrtc::FrameCryptorTransformer::MediaType::kVideoFrame :
        webrtc::FrameCryptorTransformer::MediaType::kAudioFrame,
        webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, options_.key_provider->MediaBackend()));
    cryptor->SetKeyIndex(media_send_index_->load(std::memory_order_acquire));
    cryptor->SetEnabled(true);
    cryptor->RegisterFrameCryptorTransformerObserver(webrtc::make_ref_counted<MediaState>(observation, media_state_changed_handler_));
    std::erase_if(media_observations_, [](const auto& entry) { return entry.expired(); });
    media_observations_.push_back(observation);
    return webrtc::make_ref_counted<MediaTransformer>(std::move(cryptor), std::move(observation), media_send_index_, media_owner_);
}

std::vector<std::shared_ptr<const E2eeManager::MediaObservation>> E2eeManager::media_observations() const {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<const MediaObservation>> result;
    for (const auto& entry : media_observations_) if (auto value = entry.lock()) result.push_back(std::move(value));
    return result;
}

MediaEncryptionStatus E2eeManager::ReadMediaStatus(uint64_t generation) const {
    std::lock_guard lock(mutex_);
    MediaEncryptionStatus result;
    result.native_generation = generation;
    result.policy_revision = data_packet_policy_revision_;
    result.enabled = enabled_ && options_.encryption_type == EncryptionType::GCM;
    if (!result.enabled) return result;
    const auto epoch = options_.key_provider->MediaInstallEpoch();
    result.install_epoch = epoch->load(std::memory_order_acquire);
    for (const auto& weak : media_observations_) {
        const auto binding = weak.lock();
        if (!binding || binding->generation != generation || !MediaActive(*binding)) continue;
        MediaEncryptionTrackStatus track;
        track.binding_id = binding->binding_id;
        track.participant_identity = binding->identity;
        track.track_id = binding->track;
        track.receiving = binding->receiving;
        track.video = binding->video;
        track.protected_after_current_install = !(result.install_epoch & 1) &&
            binding->frame_evidence.protected_epoch() == result.install_epoch;
        switch (binding->reported_state.load(std::memory_order_acquire)) {
        case EncryptionState::NEW: track.report = MediaCryptorReport::Waiting; break;
        case EncryptionState::OK: track.report = MediaCryptorReport::Ok; break;
        case EncryptionState::MISSING_KEY: track.report = MediaCryptorReport::MissingKey; break;
        case EncryptionState::KEY_RATCHETED: track.report = MediaCryptorReport::Ratcheted; break;
        default: track.report = MediaCryptorReport::Failed; break;
        }
        result.tracks.push_back(std::move(track));
    }
    // A concurrent explicit provider operation invalidates this sample's proof.
    if (epoch->load(std::memory_order_acquire) != result.install_epoch)
        for (auto& track : result.tracks) track.protected_after_current_install = false;
    return result;
}

FrameCryptor::FrameCryptor(const std::string& participant_identity,
                           const std::string& track_sid,
                           std::shared_ptr<KeyProvider> key_provider)
    : participant_identity_(participant_identity),
      track_sid_(track_sid),
      key_provider_(key_provider) {}

bool FrameCryptor::EncryptFrame(const std::vector<uint8_t>& unencrypted_payload, std::vector<uint8_t>& encrypted_payload) {
    if (!enabled_) {
        encrypted_payload = unencrypted_payload;
        return true;
    }

    if (!key_provider_) {
        state_ = EncryptionState::MISSING_KEY;
        return false;
    }

    std::vector<uint8_t> key = key_provider_->GetKey(participant_identity_, key_index_);
    if (key.empty()) {
        key = key_provider_->GetSharedKey(key_index_);
    }

    if (key.empty()) {
        state_ = EncryptionState::MISSING_KEY;
        return false;
    }

    // Generate 12-byte IV for AES-GCM
    uint8_t iv[12];
    RAND_bytes(iv, sizeof(iv));

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        state_ = EncryptionState::ENCRYPTION_FAILED;
        return false;
    }

    if (1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr)) {
        EVP_CIPHER_CTX_free(ctx);
        state_ = EncryptionState::ENCRYPTION_FAILED;
        return false;
    }

    if (1 != EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), iv)) {
        EVP_CIPHER_CTX_free(ctx);
        state_ = EncryptionState::ENCRYPTION_FAILED;
        return false;
    }

    encrypted_payload.resize(sizeof(iv) + unencrypted_payload.size() + 16); // IV + Ciphertext + Tag(16)
    std::memcpy(encrypted_payload.data(), iv, sizeof(iv));

    int len = 0;
    int ciphertext_len = 0;
    if (1 != EVP_EncryptUpdate(ctx, encrypted_payload.data() + sizeof(iv), &len, unencrypted_payload.data(), static_cast<int>(unencrypted_payload.size()))) {
        EVP_CIPHER_CTX_free(ctx);
        state_ = EncryptionState::ENCRYPTION_FAILED;
        return false;
    }
    ciphertext_len = len;

    if (1 != EVP_EncryptFinal_ex(ctx, encrypted_payload.data() + sizeof(iv) + ciphertext_len, &len)) {
        EVP_CIPHER_CTX_free(ctx);
        state_ = EncryptionState::ENCRYPTION_FAILED;
        return false;
    }
    ciphertext_len += len;

    uint8_t tag[16];
    if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag)) {
        EVP_CIPHER_CTX_free(ctx);
        state_ = EncryptionState::ENCRYPTION_FAILED;
        return false;
    }
    EVP_CIPHER_CTX_free(ctx);

    std::memcpy(encrypted_payload.data() + sizeof(iv) + ciphertext_len, tag, sizeof(tag));
    encrypted_payload.resize(sizeof(iv) + ciphertext_len + sizeof(tag));

    state_ = EncryptionState::OK;
    return true;
}

bool FrameCryptor::DecryptFrame(const std::vector<uint8_t>& encrypted_payload, std::vector<uint8_t>& decrypted_payload) {
    if (!enabled_) {
        decrypted_payload = encrypted_payload;
        return true;
    }

    if (encrypted_payload.size() < (12 + 16)) { // 12-byte IV + 16-byte Tag minimum
        state_ = EncryptionState::DECRYPTION_FAILED;
        return false;
    }

    if (!key_provider_) {
        state_ = EncryptionState::MISSING_KEY;
        return false;
    }

    std::vector<uint8_t> key = key_provider_->GetKey(participant_identity_, key_index_);
    if (key.empty()) {
        key = key_provider_->GetSharedKey(key_index_);
    }

    if (key.empty()) {
        state_ = EncryptionState::MISSING_KEY;
        return false;
    }

    const uint8_t* iv = encrypted_payload.data();
    size_t ciphertext_len = encrypted_payload.size() - 12 - 16;
    const uint8_t* ciphertext = encrypted_payload.data() + 12;
    const uint8_t* tag = encrypted_payload.data() + 12 + ciphertext_len;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        state_ = EncryptionState::DECRYPTION_FAILED;
        return false;
    }

    if (1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr)) {
        EVP_CIPHER_CTX_free(ctx);
        state_ = EncryptionState::DECRYPTION_FAILED;
        return false;
    }

    if (1 != EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), iv)) {
        EVP_CIPHER_CTX_free(ctx);
        state_ = EncryptionState::DECRYPTION_FAILED;
        return false;
    }

    decrypted_payload.resize(ciphertext_len);
    int len = 0;
    int plaintext_len = 0;
    if (1 != EVP_DecryptUpdate(ctx, decrypted_payload.data(), &len, ciphertext, static_cast<int>(ciphertext_len))) {
        EVP_CIPHER_CTX_free(ctx);
        state_ = EncryptionState::DECRYPTION_FAILED;
        return false;
    }
    plaintext_len = len;

    if (1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag))) {
        EVP_CIPHER_CTX_free(ctx);
        state_ = EncryptionState::DECRYPTION_FAILED;
        return false;
    }

    int ret = EVP_DecryptFinal_ex(ctx, decrypted_payload.data() + plaintext_len, &len);
    EVP_CIPHER_CTX_free(ctx);

    if (ret > 0) {
        plaintext_len += len;
        decrypted_payload.resize(plaintext_len);
        state_ = EncryptionState::OK;
        return true;
    } else {
        state_ = EncryptionState::DECRYPTION_FAILED;
        return false;
    }
}

struct DataPacketCryptor::PacketBackend {
    std::mutex mutex;
    // One cached key context: bounded even with arbitrarily many sender IDs.
    std::string identity;
    uint32_t index = 0;
    std::vector<uint8_t> material;
    webrtc::scoped_refptr<webrtc::KeyProvider> provider;
    webrtc::scoped_refptr<webrtc::DataPacketCryptor> cryptor;
};

DataPacketCryptor::DataPacketCryptor(std::shared_ptr<KeyProvider> key_provider)
    : packet_backend_(std::make_shared<PacketBackend>()),
      send_backend_(std::make_shared<PacketBackend>()),
      key_provider_(std::move(key_provider)) {}

std::variant<EncryptedDataPacket, PacketCryptoError>
DataPacketCryptor::EncryptPacket(std::string_view sender_identity, int key_index,
                                const std::vector<uint8_t>& payload) {
    // Account for both the IV and the authentication tag before encrypting.
    if (payload.size() > kMaxEncryptedPacketBytes - 12 - 16)
        return PacketCryptoError::SizeLimitExceeded;
    if (sender_identity.empty()) return PacketCryptoError::InvalidEnvelope;
    if (!key_provider_) return PacketCryptoError::MissingKey;
    try {
        const auto options = key_provider_->options();
        const int ring_size = options.key_ring_size <= 0 ? 16 :
            std::min(options.key_ring_size, 255);
        if (key_index < 0 || key_index >= ring_size)
            return PacketCryptoError::InvalidEnvelope;
        std::lock_guard lock(send_backend_->mutex);
        const std::string identity(sender_identity);
        // One material snapshot for this exact identity/slot, even during rotation.
        auto material = options.shared_key ? key_provider_->GetSharedKey(key_index)
                                          : key_provider_->GetKey(identity, key_index);
        if (material.empty()) return PacketCryptoError::MissingKey;
        auto& backend = *send_backend_;
        if (!backend.cryptor || backend.identity != identity) {
            webrtc::KeyProviderOptions native_options;
            native_options.shared_key = options.shared_key;
            native_options.key_ring_size = ring_size;
            native_options.ratchet_salt.assign(options.ratchet_salt.begin(), options.ratchet_salt.end());
            native_options.key_derivation_algorithm =
                options.key_derivation_algorithm == KeyDerivationAlgorithm::Hkdf
                    ? webrtc::kHKDF : webrtc::kPBKDF2;
            native_options.ratchet_window_size = 0;
            auto provider = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(native_options);
            auto cryptor = webrtc::make_ref_counted<webrtc::DataPacketCryptor>(
                webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, provider);
            backend.provider = std::move(provider);
            backend.cryptor = std::move(cryptor);
            backend.identity = identity;
            backend.material.clear();
        }
        if (backend.index != static_cast<uint32_t>(key_index) || backend.material != material) {
            const bool installed = options.shared_key
                ? backend.provider->SetSharedKey(key_index, material)
                : backend.provider->SetKey(identity, key_index, material);
            if (!installed) return PacketCryptoError::BackendFailure;
            backend.index = static_cast<uint32_t>(key_index);
            backend.material = std::move(material);
        }
        // Keep the sending cryptor alive across slot/material changes, preserving
        // its IV counter. Receive-side cache replacement never resets this state.
        auto sealed = backend.cryptor->Encrypt(identity, key_index, payload);
        if (!sealed.ok()) return PacketCryptoError::BackendFailure;
        const auto& result = sealed.value();
        if (result->iv.size() != 12 || result->data.size() != payload.size() + 16 ||
            result->key_index != key_index)
            return PacketCryptoError::BackendFailure;
        return EncryptedDataPacket{EncryptionType::GCM, result->key_index,
                                   result->iv, result->data};
    } catch (...) {
        return PacketCryptoError::BackendFailure;
    }
}

std::variant<AuthenticatedDataPayload, PacketCryptoError>
DataPacketCryptor::DecryptPacket(std::string_view sender_identity,
                               const EncryptedDataPacket& packet) {
    if (packet.encryption_type != EncryptionType::GCM)
        return PacketCryptoError::UnsupportedType;
    if (packet.iv.size() > kMaxEncryptedPacketBytes ||
        packet.ciphertext.size() > kMaxEncryptedPacketBytes - packet.iv.size())
        return PacketCryptoError::SizeLimitExceeded;
    if (sender_identity.empty() || packet.iv.size() != 12 ||
        packet.ciphertext.size() < 16)
        return PacketCryptoError::InvalidEnvelope;
    if (!key_provider_) return PacketCryptoError::MissingKey;

    try {
        const auto options = key_provider_->options();
        const auto ring_size = options.key_ring_size <= 0 ? 16 :
            std::min(options.key_ring_size, 255);
        // Check before narrowing: the native key ring indexes without bounds checks.
        if (packet.key_index >= static_cast<uint32_t>(ring_size))
            return PacketCryptoError::InvalidEnvelope;
        std::lock_guard lock(packet_backend_->mutex);
        const std::string identity(sender_identity);
        // Get*Key takes the provider lock and returns one consistent material snapshot.
        auto material = options.shared_key
            ? key_provider_->GetSharedKey(static_cast<int>(packet.key_index))
            : key_provider_->GetKey(identity, static_cast<int>(packet.key_index));
        if (material.empty()) return PacketCryptoError::MissingKey;
        auto& backend = *packet_backend_;
        if (!backend.cryptor || backend.identity != identity ||
            backend.index != packet.key_index || backend.material != material) {
            webrtc::KeyProviderOptions native_options;
            native_options.shared_key = options.shared_key;
            native_options.key_ring_size = ring_size;
            native_options.ratchet_salt.assign(
                options.ratchet_salt.begin(), options.ratchet_salt.end());
            native_options.key_derivation_algorithm =
                options.key_derivation_algorithm == KeyDerivationAlgorithm::Hkdf
                    ? webrtc::kHKDF : webrtc::kPBKDF2;
            // Packet reception uses explicitly installed slots. Do not silently
            // adopt the frame backend's different automatic ratchet semantics.
            native_options.ratchet_window_size = 0;
            auto provider = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(native_options);
            const bool installed = options.shared_key
                ? provider->SetSharedKey(static_cast<int>(packet.key_index), material)
                : provider->SetKey(identity, static_cast<int>(packet.key_index), material);
            if (!installed) return PacketCryptoError::BackendFailure;
            auto cryptor = webrtc::make_ref_counted<webrtc::DataPacketCryptor>(
                webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, provider);
            backend.cryptor = std::move(cryptor);
            backend.provider = std::move(provider);
            backend.identity = identity;
            backend.index = packet.key_index;
            backend.material = std::move(material);
        }
        auto envelope = webrtc::make_ref_counted<webrtc::EncryptedPacket>(
            packet.ciphertext, packet.iv, static_cast<uint8_t>(packet.key_index));
        auto result = backend.cryptor->Decrypt(identity, envelope);
        if (!result.ok()) return PacketCryptoError::AuthenticationFailed;
        return AuthenticatedDataPayload{EncryptionType::GCM, std::move(result.value())};
    } catch (...) {
        // Never expose backend exceptions, key material or unauthenticated bytes.
        return PacketCryptoError::BackendFailure;
    }
}

bool DataPacketCryptor::EncryptData(const std::vector<uint8_t>& plain_data, std::vector<uint8_t>& encrypted_data) {
    FrameCryptor cryptor("global", "data_packet", key_provider_);
    return cryptor.EncryptFrame(plain_data, encrypted_data);
}

bool DataPacketCryptor::DecryptData(const std::vector<uint8_t>& encrypted_data, std::vector<uint8_t>& decrypted_data) {
    FrameCryptor cryptor("global", "data_packet", key_provider_);
    return cryptor.DecryptFrame(encrypted_data, decrypted_data);
}

E2eeManager::E2eeManager(const E2eeOptions& options)
    : options_(options),
      enabled_(options.encryption_type != EncryptionType::NONE),
      data_packet_key_index_(options.data_packet_key_index),
      data_packet_cryptor_(std::make_shared<DataPacketCryptor>(options.key_provider)) {
    media_send_index_->store(options.media_key_index);
}

std::shared_ptr<void> E2eeManager::AcquireMediaPublishPolicy() {
    std::lock_guard lock(mutex_);
    auto leases = media_policy_leases_;
    leases->fetch_add(1);
    return std::shared_ptr<void>(nullptr, [leases](void*) { leases->fetch_sub(1); });
}

void E2eeManager::PrepareMediaSession(const std::vector<uint8_t>& sif_trailer) {
    std::lock_guard lock(mutex_);
    for (const auto& entry : media_observations_) {
        if (auto observation = entry.lock(); observation && !observation->retired.load())
            throw std::logic_error("media session preparation requires retired bindings");
    }
    if (options_.key_provider) options_.key_provider->PrepareMediaSession(sif_trailer, media_owner_);
}

bool E2eeManager::HasActiveMediaBindings() const {
    std::lock_guard lock(mutex_);
    for (const auto& entry : media_observations_)
        if (auto value = entry.lock(); value && !value->retired.load()) return true;
    return false;
}

webrtc::scoped_refptr<webrtc::FrameTransformerInterface> CreateMediaDiscarder() {
    return webrtc::make_ref_counted<MediaDiscarder>();
}

webrtc::scoped_refptr<webrtc::FrameTransformerInterface> E2eeManager::CreateReceiverCryptor(
    webrtc::Thread* signaling, const std::string& identity, const std::string& track,
    bool video, uint64_t generation, std::shared_ptr<const std::atomic<bool>> binding_active,
    std::function<void()>* retire) {
    if (!signaling || identity.empty() || track.empty() || !binding_active ||
        options_.encryption_type != EncryptionType::GCM || !options_.key_provider) return nullptr;
    std::lock_guard lock(mutex_);
    if (!enabled_) return nullptr;
    options_.key_provider->ClaimMediaOwner(media_owner_);
    auto observation = std::make_shared<MediaObservation>();
    observation->binding_id = ++next_media_binding_id_;
    observation->install_epoch = options_.key_provider->MediaInstallEpoch();
    observation->sif_trailer = options_.key_provider->MediaBackend()->options().uncrypted_magic_bytes;
    observation->track = track; observation->identity = identity;
    observation->video = video;
    observation->receiving = true;
    observation->generation = generation;
    observation->binding_active = std::move(binding_active);
    // Missing keys remain fail-closed in the backend, but the binding survives
    // so an explicit later key installation can recover without plaintext.
    auto cryptor = webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>(new webrtc::FrameCryptorTransformer(
        signaling, identity, video ? webrtc::FrameCryptorTransformer::MediaType::kVideoFrame :
        webrtc::FrameCryptorTransformer::MediaType::kAudioFrame,
        webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, options_.key_provider->MediaBackend()));
    cryptor->SetEnabled(true);
    cryptor->RegisterFrameCryptorTransformerObserver(webrtc::make_ref_counted<MediaState>(observation, media_state_changed_handler_));
    std::erase_if(media_observations_, [](const auto& entry) { return entry.expired(); });
    media_observations_.push_back(observation);
    auto transformer = webrtc::make_ref_counted<MediaTransformer>(std::move(cryptor), std::move(observation), nullptr, media_owner_);
    if (retire) *retire = [transformer] { transformer->Retire(); };
    return transformer;
}

void E2eeManager::SetEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    if (enabled && options_.encryption_type == EncryptionType::NONE)
        throw std::invalid_argument("NONE policy cannot enable encryption; install a GCM policy");
    if (enabled != enabled_ && media_policy_leases_->load() != 0)
        throw std::logic_error("media publication policy is in use");
    if (!enabled && std::any_of(media_observations_.begin(), media_observations_.end(),
        [](const auto& binding) { auto value = binding.lock(); return value && !value->retired.load(); })) {
        throw std::logic_error("protected media must stop before encryption is disabled");
    }
    if (enabled_ != enabled) ++data_packet_policy_revision_;
    enabled_ = enabled;
    for (auto& [key, cryptor] : cryptors_) {
        cryptor->set_enabled(enabled);
    }
}

E2eeManager::DataPacketState E2eeManager::data_packet_state() const {
    std::lock_guard lock(mutex_);
    return {enabled_, data_packet_key_index_, data_packet_policy_revision_};
}

bool E2eeManager::InstallRecoveryKey(const std::shared_ptr<MeetingSecretHandle>& secret) {
    if (!secret) return false;
    std::lock_guard lock(mutex_);
    if (!enabled_ || options_.encryption_type != EncryptionType::GCM ||
        !options_.key_provider || !options_.key_provider->options().shared_key ||
        data_packet_key_index_ != 0 || media_send_index_->load() != 0) {
        secret->Revoke();
        return false;
    }
    try { secret->ConsumeInto(*options_.key_provider); }
    catch (const EncryptionRequestException&) { return false; }
    ++data_packet_policy_revision_;
    return true;
}

bool E2eeManager::SetDataPacketKeyIndex(int index) {
    const int configured = options_.key_provider ? options_.key_provider->options().key_ring_size : 16;
    const int ring_size = configured <= 0 ? 16 : std::min(configured, 255);
    if (index < 0 || index >= ring_size) return false;
    std::lock_guard lock(mutex_);
    data_packet_key_index_ = index;
    return true;
}

bool E2eeManager::SetMediaKeyIndex(int index) {
    const int configured = options_.key_provider ? options_.key_provider->options().key_ring_size : 16;
    if (index < 0 || index >= configured) return false;
    media_send_index_->store(index, std::memory_order_release);
    return true;
}

std::shared_ptr<FrameCryptor> E2eeManager::GetCryptor(const std::string& participant_identity, const std::string& track_sid) {
    std::lock_guard lock(mutex_);
    auto key = std::make_pair(participant_identity, track_sid);
    auto it = cryptors_.find(key);
    if (it != cryptors_.end()) {
        return it->second;
    }

    auto cryptor = std::make_shared<FrameCryptor>(participant_identity, track_sid, options_.key_provider);
    cryptor->set_enabled(enabled_);
    cryptors_[key] = cryptor;
    return cryptor;
}

} // namespace livekit
