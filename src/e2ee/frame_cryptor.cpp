#include "frame_cryptor.h"
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

DataPacketCryptor::DataPacketCryptor(std::shared_ptr<KeyProvider> key_provider)
    : key_provider_(key_provider) {}

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
      data_packet_cryptor_(std::make_shared<DataPacketCryptor>(options.key_provider)) {}

void E2eeManager::SetEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    enabled_ = enabled;
    for (auto& [key, cryptor] : cryptors_) {
        cryptor->set_enabled(enabled);
    }
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
