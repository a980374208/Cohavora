#pragma once

#include "meeting_encryption.h"

namespace livekit {

// The UI/session owner retains reusable key material. Meetings only receive
// independent, one-use handles, so consuming or revoking a meeting request
// never retires the configured key. This object has no raw-key accessor.
class MeetingEncryptionKey final {
public:
    static std::unique_ptr<MeetingEncryptionKey> Create(std::vector<uint8_t> material) {
        MarkSensitiveMemoryUsed();
        try {
            auto key = std::unique_ptr<MeetingEncryptionKey>(
                new MeetingEncryptionKey(std::move(material)));
            if (key->material_.empty() || key->material_.size() > 4096 ||
                !std::all_of(key->material_.begin(), key->material_.end(), [](uint8_t value) {
                    return value >= 0x20 && value <= 0x7e;
                })) {
                throw EncryptionRequestException(EncryptionRequestError::InvalidKeyMaterial);
            }
            return key;
        } catch (...) {
            // If allocating the holder failed before ownership transferred,
            // the input vector still belongs to this frame.
            if (!material.empty()) OPENSSL_cleanse(material.data(), material.size());
            throw;
        }
    }

    ~MeetingEncryptionKey() {
        if (!material_.empty()) OPENSSL_cleanse(material_.data(), material_.size());
    }
    MeetingEncryptionKey(const MeetingEncryptionKey&) = delete;
    MeetingEncryptionKey& operator=(const MeetingEncryptionKey&) = delete;

    std::shared_ptr<MeetingSecretHandle> CreateMeetingSecret() const {
        return MeetingSecretHandle::Create(material_, KeyMaterialFormat::AsciiText);
    }

private:
    explicit MeetingEncryptionKey(std::vector<uint8_t> material)
        : material_(std::move(material)) {}

    std::vector<uint8_t> material_;
};

} // namespace livekit
