#include "key_provider.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <iostream>

namespace livekit {

KeyProvider::KeyProvider(const KeyProviderOptions& options)
    : options_(options) {}

void KeyProvider::SetSharedKey(const std::vector<uint8_t>& key, int key_index) {
    std::lock_guard lock(mutex_);
    shared_keys_[key_index] = key;
}

std::vector<uint8_t> KeyProvider::GetSharedKey(int key_index) const {
    std::lock_guard lock(mutex_);
    auto it = shared_keys_.find(key_index);
    if (it != shared_keys_.end()) {
        return it->second;
    }
    return {};
}

std::vector<uint8_t> KeyProvider::DeriveKey(const std::vector<uint8_t>& base_key, const std::string& salt) const {
    if (base_key.empty()) return {};

    std::vector<uint8_t> derived_key(32); // 256-bit key
    PKCS5_PBKDF2_HMAC(
        reinterpret_cast<const char*>(base_key.data()), static_cast<int>(base_key.size()),
        reinterpret_cast<const uint8_t*>(salt.data()), static_cast<int>(salt.size()),
        1000, EVP_sha256(),
        static_cast<int>(derived_key.size()), derived_key.data()
    );
    return derived_key;
}

std::vector<uint8_t> KeyProvider::RatchetSharedKey(int key_index) {
    std::lock_guard lock(mutex_);
    auto it = shared_keys_.find(key_index);
    if (it != shared_keys_.end() && !it->second.empty()) {
        std::vector<uint8_t> ratcheted = DeriveKey(it->second, options_.ratchet_salt);
        shared_keys_[key_index + 1] = ratcheted;
        return ratcheted;
    }
    return {};
}

bool KeyProvider::SetKey(const std::string& participant_identity, int key_index, const std::vector<uint8_t>& key) {
    std::lock_guard lock(mutex_);
    participant_keys_[participant_identity][key_index] = key;
    return true;
}

std::vector<uint8_t> KeyProvider::GetKey(const std::string& participant_identity, int key_index) const {
    std::lock_guard lock(mutex_);
    auto p_it = participant_keys_.find(participant_identity);
    if (p_it != participant_keys_.end()) {
        auto k_it = p_it->second.find(key_index);
        if (k_it != p_it->second.end()) {
            return k_it->second;
        }
    }
    return {};
}

std::vector<uint8_t> KeyProvider::RatchetKey(const std::string& participant_identity, int key_index) {
    std::lock_guard lock(mutex_);
    auto p_it = participant_keys_.find(participant_identity);
    if (p_it != participant_keys_.end()) {
        auto k_it = p_it->second.find(key_index);
        if (k_it != p_it->second.end() && !k_it->second.empty()) {
            std::vector<uint8_t> ratcheted = DeriveKey(k_it->second, options_.ratchet_salt);
            p_it->second[key_index + 1] = ratcheted;
            return ratcheted;
        }
    }
    return {};
}

} // namespace livekit
