#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <functional>
#include <mutex>
#include <string_view>
#include <variant>
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
    // Explicit DataPacket send slot; independent of media cryptors and receive-key installation.
    int data_packet_key_index{0};
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

enum class PacketCryptoError {
    UnsupportedType, InvalidEnvelope, MissingKey,
    AuthenticationFailed, SizeLimitExceeded, BackendFailure
};

struct EncryptedDataPacket {
    EncryptionType encryption_type{EncryptionType::NONE};
    uint32_t key_index{0};
    std::vector<uint8_t> iv;
    std::vector<uint8_t> ciphertext;
};

struct AuthenticatedDataPayload {
    EncryptionType encryption_type{EncryptionType::NONE};
    std::vector<uint8_t> bytes; // Encoded EncryptedPacketPayload, authenticated.
};

class DataPacketCryptor {
public:
    explicit DataPacketCryptor(std::shared_ptr<KeyProvider> key_provider);

    // Local packet envelope limit, including IV and GCM tag. This comfortably
    // fits the 15 KB stream chunks; it is not a total stream transfer limit.
    static constexpr size_t kMaxEncryptedPacketBytes = 64 * 1024;
    std::variant<AuthenticatedDataPayload, PacketCryptoError> DecryptPacket(
        std::string_view sender_identity, const EncryptedDataPacket& packet);
    std::variant<EncryptedDataPacket, PacketCryptoError> EncryptPacket(
        std::string_view sender_identity, int key_index,
        const std::vector<uint8_t>& payload);

    // Legacy AES-256 helper and combined IV/ciphertext/tag format are unchanged.
    bool EncryptData(const std::vector<uint8_t>& plain_data, std::vector<uint8_t>& encrypted_data);
    bool DecryptData(const std::vector<uint8_t>& encrypted_data, std::vector<uint8_t>& decrypted_data);

private:
    struct PacketBackend;
    std::shared_ptr<PacketBackend> packet_backend_;
    std::shared_ptr<PacketBackend> send_backend_;
    std::shared_ptr<KeyProvider> key_provider_;
};

class E2eeManager {
public:
    using StateChangedHandler = std::function<void(const std::string& participant_identity, EncryptionState state)>;

    explicit E2eeManager(const E2eeOptions& options);

    void SetEnabled(bool enabled);
    bool enabled() const { std::lock_guard lock(mutex_); return enabled_; }
    EncryptionType encryption_type() const { return options_.encryption_type; }
    struct DataPacketState {
        bool enabled;
        int key_index;
        uint64_t policy_revision;
    };
    DataPacketState data_packet_state() const;
    // Explicit send selection; installing receive keys does not change it.
    // Invalid ring indexes return false without changing the selected slot.
    bool SetDataPacketKeyIndex(int index);

    std::shared_ptr<FrameCryptor> GetCryptor(const std::string& participant_identity, const std::string& track_sid);
    std::shared_ptr<DataPacketCryptor> data_packet_cryptor() const { return data_packet_cryptor_; }

    void RatchetKey() {
        if (options_.key_provider) {
            options_.key_provider->RatchetSharedKey();
        }
    }

    void SetStateChangedHandler(StateChangedHandler handler) {
        state_changed_handler_ = std::move(handler);
    }

private:
    E2eeOptions options_;
    bool enabled_{true};
    int data_packet_key_index_{0};
    uint64_t data_packet_policy_revision_{0};
    std::shared_ptr<DataPacketCryptor> data_packet_cryptor_;
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, std::shared_ptr<FrameCryptor>> cryptors_;
    StateChangedHandler state_changed_handler_;
};

} // namespace livekit
