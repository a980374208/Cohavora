#pragma once

#include "key_provider.h"
#include "../core/sensitive_memory_policy.h"
#include <openssl/crypto.h>
#include <stdexcept>
#include <utility>
#include <algorithm>

namespace livekit {

enum class MeetingEncryptionMode { Off, Required };
enum class KeyMaterialFormat { AsciiText, Raw32 };
enum class EncryptionRequestError { MissingKey, InvalidKeyMaterial, StaleContext, InvalidPolicy };

class EncryptionRequestException final : public std::runtime_error {
public:
    explicit EncryptionRequestException(EncryptionRequestError code)
        : std::runtime_error("Meeting encryption request is not usable"), code_(code) {}
    EncryptionRequestError code() const noexcept { return code_; }
private:
    EncryptionRequestError code_;
};

// Copies of a request share revocation and consumption, never the key bytes.
// No serializer, raw-key accessor, diagnostic string, or settings adapter.
class MeetingSecretHandle final {
public:
    static std::shared_ptr<MeetingSecretHandle> Create(
        std::vector<uint8_t> material, KeyMaterialFormat format = KeyMaterialFormat::AsciiText) {
        MarkSensitiveMemoryUsed();
        auto result = std::shared_ptr<MeetingSecretHandle>(new MeetingSecretHandle(std::move(material)));
        if ((format != KeyMaterialFormat::AsciiText && format != KeyMaterialFormat::Raw32) ||
            result->material_.empty() || result->material_.size() > 4096 ||
            (format == KeyMaterialFormat::Raw32 && result->material_.size() != 32) ||
            (format == KeyMaterialFormat::AsciiText && !ValidAsciiText(result->material_)))
            throw EncryptionRequestException(EncryptionRequestError::InvalidKeyMaterial);
        return result;
    }
    ~MeetingSecretHandle() { Clear(); }
    MeetingSecretHandle(const MeetingSecretHandle&) = delete;
    MeetingSecretHandle& operator=(const MeetingSecretHandle&) = delete;

    bool available() const {
        std::lock_guard lock(mutex_);
        return !material_.empty();
    }
    void Revoke() {
        std::lock_guard lock(mutex_);
        Clear();
    }
    std::shared_ptr<KeyProvider> ConsumeProvider() {
        try {
            KeyProviderOptions options;
            options.shared_key = true;
            auto provider = std::make_shared<KeyProvider>(options);
            ConsumeInto(*provider);
            return provider;
        } catch (...) { Revoke(); throw; }
    }
    void ConsumeInto(KeyProvider& provider) {
        std::vector<uint8_t> material;
        {
            std::lock_guard lock(mutex_);
            if (material_.empty()) throw EncryptionRequestException(EncryptionRequestError::StaleContext);
            // Consumption is the linearization point. Revocation must never
            // block the Qt owner on backend KDF/installation already in flight.
            material.swap(material_);
        }
        const auto wipe = [&] {
            if (!material.empty()) OPENSSL_cleanse(material.data(), material.size());
        };
        try {
            if (!provider.options().shared_key)
                throw EncryptionRequestException(EncryptionRequestError::InvalidPolicy);
            provider.SetSharedKey(material);
            wipe();
        } catch (...) {
            wipe();
            throw;
        }
    }
private:
    static bool ValidAsciiText(const std::vector<uint8_t>& bytes) {
        // Flutter String.codeUnits truncates non-ASCII code units whereas Qt
        // toUtf8 encodes them. The product interoperability profile is printable
        // ASCII only; do not normalize, trim, or silently alter shared material.
        return std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) {
            return value >= 0x20 && value <= 0x7e;
        });
    }
    explicit MeetingSecretHandle(std::vector<uint8_t> material) : material_(std::move(material)) {}
    void Clear() noexcept {
        if (!material_.empty()) OPENSSL_cleanse(material_.data(), material_.size());
        material_.clear();
    }
    mutable std::mutex mutex_;
    std::vector<uint8_t> material_;
};

// First product profile: shared slot 0, PBKDF2, official native protocol defaults.
// This object is intentionally separate from persisted MediaPreferences.
struct MeetingEncryptionRequest {
    MeetingEncryptionMode mode = MeetingEncryptionMode::Off;
    std::shared_ptr<MeetingSecretHandle> secret;

    void Validate() const {
        if (mode == MeetingEncryptionMode::Off && !secret) return;
        if (mode != MeetingEncryptionMode::Required)
            throw EncryptionRequestException(EncryptionRequestError::InvalidPolicy);
        if (!secret) throw EncryptionRequestException(EncryptionRequestError::MissingKey);
        if (!secret->available()) throw EncryptionRequestException(EncryptionRequestError::StaleContext);
    }
    void Revoke() { if (secret) secret->Revoke(); secret.reset(); }
};

} // namespace livekit
