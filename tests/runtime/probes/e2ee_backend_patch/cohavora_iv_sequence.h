#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace cohavora_e2ee {

// One instance per loaded backend, shared by media and data senders. The
// production caller seeds the full 96-bit IV with BoringSSL's CSPRNG once.
// Subsequent IVs increment that field; transport/key/cryptor replacement does
// not reset it. Independent processes retain the probabilistic separation of
// random 96-bit starting points, not a global cross-device uniqueness proof.
class IvSequence final {
 public:
  using Iv = std::array<uint8_t, 12>;
  using Seed = bool (*)(Iv&);
  static constexpr uint64_t kMaximumInvocations = uint64_t{1} << 32;

  explicit IvSequence(Seed seed, uint64_t limit = kMaximumInvocations)
      : seed_(seed), remaining_(limit <= kMaximumInvocations ? limit : 0) {}

  bool Next(Iv& output) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      initialized_ = true;
      if (!seed_ || !seed_(next_)) remaining_ = 0;
    }
    if (remaining_ == 0) return false;
    output = next_;
    --remaining_;
    for (std::size_t i = next_.size(); i != 0; --i) {
      if (++next_[i - 1] != 0) break;
    }
    return true;
  }

 private:
  std::mutex mutex_;
  Seed seed_;
  Iv next_{};
  uint64_t remaining_;
  bool initialized_ = false;
};

}  // namespace cohavora_e2ee
