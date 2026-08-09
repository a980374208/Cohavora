#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <cstdint>

namespace livekit {

enum class KeyDerivationAlgorithm {
    PBKDF2,
    HKDF
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

    void SetSharedKey(const std::vector<uint8_t>& key, int key_index = 0);
    std::vector<uint8_t> GetSharedKey(int key_index = 0) const;
    std::vector<uint8_t> RatchetSharedKey(int key_index = 0);

    bool SetKey(const std::string& participant_identity, int key_index, const std::vector<uint8_t>& key);
    std::vector<uint8_t> GetKey(const std::string& participant_identity, int key_index = 0) const;
    std::vector<uint8_t> RatchetKey(const std::string& participant_identity, int key_index = 0);

    KeyProviderOptions options() const { return options_; }

private:
    std::vector<uint8_t> DeriveKey(const std::vector<uint8_t>& base_key, const std::string& salt) const;

private:
    KeyProviderOptions options_;
    mutable std::mutex mutex_;
    std::map<int, std::vector<uint8_t>> shared_keys_;
    std::map<std::string, std::map<int, std::vector<uint8_t>>> participant_keys_;
};

} // namespace livekit
