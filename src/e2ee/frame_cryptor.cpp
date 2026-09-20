#include "frame_cryptor.h"
#include "api/crypto/frame_crypto_transformer.h"
#include <algorithm>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <iostream>

namespace livekit {

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
                options.key_derivation_algorithm == KeyDerivationAlgorithm::HKDF
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
                options.key_derivation_algorithm == KeyDerivationAlgorithm::HKDF
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
      data_packet_key_index_(options.data_packet_key_index),
      data_packet_cryptor_(std::make_shared<DataPacketCryptor>(options.key_provider)) {}

void E2eeManager::SetEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
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

bool E2eeManager::SetDataPacketKeyIndex(int index) {
    const int configured = options_.key_provider ? options_.key_provider->options().key_ring_size : 16;
    const int ring_size = configured <= 0 ? 16 : std::min(configured, 255);
    if (index < 0 || index >= ring_size) return false;
    std::lock_guard lock(mutex_);
    data_packet_key_index_ = index;
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
