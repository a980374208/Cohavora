#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <cstdint>
#include <atomic>
#include "api/crypto/frame_crypto_transformer.h"

namespace livekit {

enum class KeyDerivationAlgorithm {
    PBKDF2,
    Hkdf
};

struct KeyProviderOptions {
    int ratchet_window_size{16};
    std::string ratchet_salt{"LKFrameEncryptionKey"};
    int failure_tolerance{-1};
    int key_ring_size{16};
    KeyDerivationAlgorithm key_derivation_algorithm{KeyDerivationAlgorithm::PBKDF2};
    bool shared_key{false};
};

class KeyProvider {
public:
    explicit KeyProvider(const KeyProviderOptions& options = KeyProviderOptions());
    ~KeyProvider();

    // Empty material clears data-only keys. After media backend creation it is
    // rejected without mutation: media revocation requires retiring bindings.
    void SetSharedKey(const std::vector<uint8_t>& key, int key_index = 0);
    std::vector<uint8_t> GetSharedKey(int key_index = 0) const;
    std::vector<uint8_t> RatchetSharedKey(int key_index = 0);

    bool SetKey(const std::string& participant_identity, int key_index, const std::vector<uint8_t>& key);
    std::vector<uint8_t> GetKey(const std::string& participant_identity, int key_index = 0) const;
    std::vector<uint8_t> RatchetKey(const std::string& participant_identity, int key_index = 0);

    KeyProviderOptions options() const { return options_; }
    // Lazily created backend shared by this provider's media bindings. Key
    // installation stays behind the provider lock; callers never own raw keys.
    webrtc::scoped_refptr<webrtc::KeyProvider> MediaBackend();
    // Even values identify completed explicit installations; odd means a
    // mutation is in progress. This is not the backend's implicit ratchet index.
    std::shared_ptr<const std::atomic<uint64_t>> MediaInstallEpoch() const { return media_install_epoch_; }
    // Called by the manager before creating this session's media bindings.
    // Replaces the backend instead of mutating options read by crypto threads.
private:
    friend class E2eeManager;
    void PrepareMediaSession(const std::vector<uint8_t>& sif_trailer,
                             const std::shared_ptr<void>& owner);
    void ClaimMediaOwner(const std::shared_ptr<void>& owner);
    std::weak_ptr<void> media_owner_;

private:
    std::vector<uint8_t> DeriveKey(const std::vector<uint8_t>& base_key, const std::string& salt) const;

private:
    KeyProviderOptions options_;
    mutable std::mutex mutex_;
    std::map<int, std::vector<uint8_t>> shared_keys_;
    std::map<std::string, std::map<int, std::vector<uint8_t>>> participant_keys_;
    webrtc::scoped_refptr<webrtc::DefaultKeyProviderImpl> media_backend_;
    std::vector<uint8_t> sif_trailer_;
    std::shared_ptr<std::atomic<uint64_t>> media_install_epoch_ = std::make_shared<std::atomic<uint64_t>>(2);
};

} // namespace livekit
