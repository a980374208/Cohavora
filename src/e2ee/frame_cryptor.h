#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>
#include <functional>
#include <mutex>
#include <string_view>
#include <variant>
#include <atomic>
#include "key_provider.h"
#include "media_encryption_status.h"
#include "media_frame_evidence.h"

namespace livekit {
class MeetingSecretHandle;

// Keep a receiver fail-closed while retiring its cryptor; never clear the hook
// to nullptr while the underlying receive channel can still deliver frames.
webrtc::scoped_refptr<webrtc::FrameTransformerInterface> CreateMediaDiscarder();

// Stable native hook: drops input until Room commits a receiver cryptor.
// The receiver owns the hook; Room keeps only a weak lookup for duplicate events.
class MediaReceiverGate {
public:
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> Hook();
    bool SetTarget(uint64_t binding_serial, webrtc::scoped_refptr<webrtc::FrameTransformerInterface> target);
    void Transform(std::unique_ptr<webrtc::TransformableFrameInterface> frame);
    void Register(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback);
    void Register(webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback, uint32_t ssrc);
    void Unregister();
    void Unregister(uint32_t ssrc);
private:
    friend std::shared_ptr<MediaReceiverGate> CreateMediaReceiverGate();
    std::weak_ptr<MediaReceiverGate> self_;
    std::mutex mutex_;
    uint64_t binding_serial_ = 0;
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> target_;
    webrtc::scoped_refptr<webrtc::TransformedFrameCallback> callback_;
    std::map<uint32_t, webrtc::scoped_refptr<webrtc::TransformedFrameCallback>> sinks_;
};
std::shared_ptr<MediaReceiverGate> CreateMediaReceiverGate();

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
    INTERNAL_ERROR,
    KEY_RATCHETED
};

struct E2eeOptions {
    EncryptionType encryption_type{EncryptionType::GCM};
    std::shared_ptr<KeyProvider> key_provider;
    // Explicit DataPacket send slot; independent of media cryptors and receive-key installation.
    int data_packet_key_index{0};
    int media_key_index{0};
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
    struct MediaObservation {
        uint64_t binding_id = 0;
        std::string track;
        std::string identity;
        bool video = false;
        bool receiving = false;
        std::atomic<bool> retired{false};
        std::shared_ptr<const std::atomic<bool>> binding_active;
        unsigned expected_codec = 0;
        uint64_t generation = 0;
        std::atomic<uint64_t> input{0}, output{0};
        std::atomic<unsigned> codec_mask{0};
        std::atomic<int> state{0};
        std::atomic<EncryptionState> reported_state{EncryptionState::NEW};
        std::shared_ptr<const std::atomic<uint64_t>> install_epoch;
        std::vector<uint8_t> sif_trailer;
        MediaFrameEvidence frame_evidence;
    };
    using MediaStateChangedHandler = std::function<void(
        std::shared_ptr<const MediaObservation>, EncryptionState)>;
    // Configure before bindings are created. Each binding captures this handler.
    void SetMediaStateChangedHandler(MediaStateChangedHandler handler) {
        std::lock_guard lock(mutex_);
        media_state_changed_handler_ = std::move(handler);
    }
    // Initial media admission, before signaling or native sender side effects.
    bool CanPublishMedia(const std::string& identity, const std::string& codec) const;
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> CreateSenderCryptor(
        webrtc::Thread* signaling, const std::string& identity,
        const std::string& track, const std::string& codec, uint64_t generation);
    webrtc::scoped_refptr<webrtc::FrameTransformerInterface> CreateReceiverCryptor(
        webrtc::Thread* signaling, const std::string& identity,
        const std::string& track, bool video, uint64_t generation,
        std::shared_ptr<const std::atomic<bool>> binding_active,
        std::function<void()>* retire = nullptr);
    std::vector<std::shared_ptr<const MediaObservation>> media_observations() const;
    MediaEncryptionStatus ReadMediaStatus(uint64_t generation) const;
    bool HasActiveMediaBindings() const;
    void PrepareMediaSession(const std::vector<uint8_t>& sif_trailer);
    using StateChangedHandler = std::function<void(const std::string& participant_identity, EncryptionState state)>;

    explicit E2eeManager(const E2eeOptions& options);

    void SetEnabled(bool enabled);
    // Pins the on/off policy from publish planning through sender commit.
    std::shared_ptr<void> AcquireMediaPublishPolicy();
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
    bool SetMediaKeyIndex(int index);
    // Product recovery for the shared-slot-0 profile. Advances delivery policy
    // so transfers and RPC admitted under the previous key cannot finish later.
    bool InstallRecoveryKey(const std::shared_ptr<MeetingSecretHandle>& secret);

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
    std::shared_ptr<void> media_owner_ = std::make_shared<int>(0);
    bool enabled_{true};
    int data_packet_key_index_{0};
    uint64_t data_packet_policy_revision_{0};
    std::shared_ptr<DataPacketCryptor> data_packet_cryptor_;
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, std::shared_ptr<FrameCryptor>> cryptors_;
    StateChangedHandler state_changed_handler_;
    MediaStateChangedHandler media_state_changed_handler_;
    std::vector<std::weak_ptr<MediaObservation>> media_observations_;
    uint64_t next_media_binding_id_ = 0;
    std::shared_ptr<std::atomic<int>> media_send_index_ = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<unsigned>> media_policy_leases_ = std::make_shared<std::atomic<unsigned>>(0);
};

} // namespace livekit
