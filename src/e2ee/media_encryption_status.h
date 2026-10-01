#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace livekit {

// Backend state reports and explicit-install frame evidence are separate. The
// observer has no key epoch; only the bounded input/completion ledger establishes
// protected_after_current_install. Neither asserts room-wide protection.
enum class MediaCryptorReport { Waiting, Ok, MissingKey, Failed, Ratcheted };

struct MediaEncryptionTrackStatus {
    uint64_t binding_id = 0;
    std::string participant_identity;
    std::string track_id;
    bool receiving = false;
    bool video = false;
    MediaCryptorReport report = MediaCryptorReport::Waiting;
    // A non-SIF frame completed crypto after the current explicit installation
    // began and finished, with no intervening install. No room-wide assertion.
    bool protected_after_current_install = false;
};

struct MediaEncryptionStatus {
    uint64_t native_generation = 0;
    // Delivery-policy revision fences recovery results; it is not a media key epoch.
    uint64_t policy_revision = 0;
    bool enabled = false;
    uint64_t install_epoch = 0;
    std::vector<MediaEncryptionTrackStatus> tracks;
};

} // namespace livekit
