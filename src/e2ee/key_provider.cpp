#include "key_provider.h"
#include <algorithm>
#include <stdexcept>
#include <openssl/crypto.h>
#include "api/make_ref_counted.h"

namespace livekit {
namespace {
class KeyMutationEpoch final {
public:
    explicit KeyMutationEpoch(std::atomic<uint64_t>& epoch) : epoch_(epoch) {
        const auto before = epoch_.load(std::memory_order_relaxed);
        epoch_.store(before + ((before & 1) ? 2 : 1), std::memory_order_release);
    }
    // An exceptional/failed install remains unknown (odd), even if the backend
    // was partially changed. The next successful installation can recover it.
    void Commit() { epoch_.fetch_add(1, std::memory_order_release); }
private:
    std::atomic<uint64_t>& epoch_;
};
void ClearMaterial(std::vector<uint8_t>& material) noexcept {
    if (!material.empty()) OPENSSL_cleanse(material.data(), material.size());
}
void ReplaceMaterial(std::vector<uint8_t>& target, const std::vector<uint8_t>& source) {
    // Allocate before wiping: allocation failure must leave the installed key intact.
    auto replacement = source;
    ClearMaterial(target);
    target.swap(replacement);
}
}


KeyProvider::KeyProvider(const KeyProviderOptions& options)
    : options_(options) {
    if (options_.key_ring_size < 1 || options_.key_ring_size > 255)
        throw std::invalid_argument("key ring size must be between 1 and 255");
    if (options_.ratchet_window_size < 0 || options_.failure_tolerance < -1)
        throw std::invalid_argument("invalid ratchet window or failure tolerance");
    if (options_.key_derivation_algorithm != KeyDerivationAlgorithm::PBKDF2 &&
        options_.key_derivation_algorithm != KeyDerivationAlgorithm::Hkdf)
        throw std::invalid_argument("unsupported key derivation algorithm");
}

KeyProvider::~KeyProvider() {
    // Only owned buffers can be wiped here; backend/caller copies have independent lifetimes.
    for (auto& [index, key] : shared_keys_) ClearMaterial(key);
    for (auto& [identity, slots] : participant_keys_)
        for (auto& [index, key] : slots) ClearMaterial(key);
}

void KeyProvider::SetSharedKey(const std::vector<uint8_t>& key, int key_index) {
    if (key_index < 0 || key_index >= options_.key_ring_size) throw std::out_of_range("key index outside ring");
    std::lock_guard lock(mutex_);
    if (key.empty() && media_backend_) throw std::invalid_argument("media key removal requires retiring bindings");
    KeyMutationEpoch epoch(*media_install_epoch_);
    ReplaceMaterial(shared_keys_[key_index], key);
    if (!media_backend_ || media_backend_->SetSharedKey(key_index, key)) epoch.Commit();
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

    webrtc::KeyProviderOptions native;
    native.ratchet_salt.assign(salt.begin(), salt.end());
    native.key_derivation_algorithm = options_.key_derivation_algorithm == KeyDerivationAlgorithm::Hkdf
        ? webrtc::kHKDF : webrtc::kPBKDF2;
    auto provider = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(native);
    auto handler = webrtc::make_ref_counted<webrtc::ParticipantKeyHandler>(provider.get());
    return handler->RatchetKeyMaterial(base_key);
}

std::vector<uint8_t> KeyProvider::RatchetSharedKey(int key_index) {
    std::lock_guard lock(mutex_);
    auto it = shared_keys_.find(key_index);
    if (it != shared_keys_.end() && !it->second.empty()) {
        std::vector<uint8_t> ratcheted = DeriveKey(it->second, options_.ratchet_salt);
        if (ratcheted.empty()) return {};
        KeyMutationEpoch epoch(*media_install_epoch_);
        ReplaceMaterial(shared_keys_[key_index], ratcheted);
        if (!media_backend_ || media_backend_->SetSharedKey(key_index, ratcheted)) epoch.Commit();
        return ratcheted;
    }
    return {};
}

bool KeyProvider::SetKey(const std::string& participant_identity, int key_index, const std::vector<uint8_t>& key) {
    if (key_index < 0 || key_index >= options_.key_ring_size || participant_identity.empty()) return false;
    std::lock_guard lock(mutex_);
    if (key.empty() && media_backend_) return false;
    KeyMutationEpoch epoch(*media_install_epoch_);
    ReplaceMaterial(participant_keys_[participant_identity][key_index], key);
    if (!media_backend_ || media_backend_->SetKey(participant_identity, key_index, key)) epoch.Commit();
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
            if (ratcheted.empty()) return {};
            KeyMutationEpoch epoch(*media_install_epoch_);
            ReplaceMaterial(p_it->second[key_index], ratcheted);
            if (!media_backend_ || media_backend_->SetKey(participant_identity, key_index, ratcheted)) epoch.Commit();
            return ratcheted;
        }
    }
    return {};
}

void KeyProvider::ClaimMediaOwner(const std::shared_ptr<void>& owner) {
    std::lock_guard lock(mutex_);
    if (auto current = media_owner_.lock(); current && current != owner)
        throw std::logic_error("media provider already belongs to another manager");
    media_owner_ = owner;
}

void KeyProvider::PrepareMediaSession(const std::vector<uint8_t>& sif_trailer,
                                    const std::shared_ptr<void>& owner) {
    std::lock_guard lock(mutex_);
    if (auto current = media_owner_.lock(); current && current != owner)
        throw std::logic_error("media provider already belongs to another manager");
    media_owner_ = owner;
    if (sif_trailer_ == sif_trailer) return;
    sif_trailer_ = sif_trailer;
    media_backend_ = nullptr;
}

webrtc::scoped_refptr<webrtc::KeyProvider> KeyProvider::MediaBackend() {
    std::lock_guard lock(mutex_);
    if (!media_backend_) {
        webrtc::KeyProviderOptions native;
        native.shared_key = options_.shared_key;
        native.ratchet_salt.assign(options_.ratchet_salt.begin(), options_.ratchet_salt.end());
        native.ratchet_window_size = options_.ratchet_window_size;
        native.failure_tolerance = options_.failure_tolerance;
        native.uncrypted_magic_bytes = sif_trailer_;
        native.key_ring_size = options_.key_ring_size;
        native.key_derivation_algorithm = options_.key_derivation_algorithm == KeyDerivationAlgorithm::Hkdf ? webrtc::kHKDF : webrtc::kPBKDF2;
        auto backend = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(native);
        backend->options().discard_frame_when_cryptor_not_ready = true;
        for (const auto& [index, material] : shared_keys_)
            if (!material.empty()) backend->SetSharedKey(index, material);
        for (const auto& [identity, slots] : participant_keys_)
            for (const auto& [index, material] : slots)
                if (!material.empty()) backend->SetKey(identity, index, material);
        media_backend_ = std::move(backend);
    }
    return media_backend_;
}

} // namespace livekit
