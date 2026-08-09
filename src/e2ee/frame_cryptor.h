#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <functional>
#include <mutex>
#include "key_provider.h"

namespace livekit {

enum class EncryptionType {
    NONE,
    GCM,
    CUSTOM
};

enum class EncryptionState {
    NEW,
    OK,
    ENCRYPTION_FAILED,
    DECRYPTION_FAILED,
    MISSING_KEY,
    INTERNAL_ERROR
};

struct E2eeOptions {
    EncryptionType encryption_type{EncryptionType::GCM};
    std::shared_ptr<KeyProvider> key_provider;
};

class FrameCryptor {
public:
    FrameCryptor(const std::string& participant_identity,
                 const std::string& track_sid,
                 std::shared_ptr<KeyProvider> key_provider);

    bool enabled() const { return enabled_; }
    void set_enabled(bool enabled) { enabled_ = enabled; }

    EncryptionState state() const { return state_; }
    void set_key_index(int index) { key_index_ = index; }

    // AES-256-GCM Encrypt Frame Payload
    bool EncryptFrame(const std::vector<uint8_t>& unencrypted_payload, std::vector<uint8_t>& encrypted_payload);

    // AES-256-GCM Decrypt Frame Payload
    bool DecryptFrame(const std::vector<uint8_t>& encrypted_payload, std::vector<uint8_t>& decrypted_payload);

private:
    std::string participant_identity_;
    std::string track_sid_;
    std::shared_ptr<KeyProvider> key_provider_;
    bool enabled_{true};
    int key_index_{0};
    EncryptionState state_{EncryptionState::NEW};
};

class DataPacketCryptor {
public:
    explicit DataPacketCryptor(std::shared_ptr<KeyProvider> key_provider);

    bool EncryptData(const std::vector<uint8_t>& plain_data, std::vector<uint8_t>& encrypted_data);
    bool DecryptData(const std::vector<uint8_t>& encrypted_data, std::vector<uint8_t>& decrypted_data);

private:
    std::shared_ptr<KeyProvider> key_provider_;
};

class E2eeManager {
public:
    using StateChangedHandler = std::function<void(const std::string& participant_identity, EncryptionState state)>;

    explicit E2eeManager(const E2eeOptions& options);

    void SetEnabled(bool enabled);
    bool enabled() const { return enabled_; }

    std::shared_ptr<FrameCryptor> GetCryptor(const std::string& participant_identity, const std::string& track_sid);
    std::shared_ptr<DataPacketCryptor> data_packet_cryptor() const { return data_packet_cryptor_; }

    void SetStateChangedHandler(StateChangedHandler handler) {
        state_changed_handler_ = std::move(handler);
    }

private:
    E2eeOptions options_;
    bool enabled_{true};
    std::shared_ptr<DataPacketCryptor> data_packet_cryptor_;
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, std::shared_ptr<FrameCryptor>> cryptors_;
    StateChangedHandler state_changed_handler_;
};

} // namespace livekit
