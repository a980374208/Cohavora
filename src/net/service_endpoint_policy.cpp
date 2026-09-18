#include "src/net/service_endpoint_policy.h"

#include <QtCore/QtGlobal>

#include <atomic>

namespace OpenMeeting {
namespace {
constexpr int kUninitialized = -1;
constexpr int kStrictTransport = 0;
constexpr int kDebugHttpTransport = 1;

std::atomic<int> gTransportMode{kUninitialized};
} // namespace

void initializeServiceEndpointPolicy(bool debugHttpEnabled) {
    const int requested = debugHttpEnabled ? kDebugHttpTransport : kStrictTransport;
    int expected = kUninitialized;
    if (!gTransportMode.compare_exchange_strong(
            expected, requested, std::memory_order_release, std::memory_order_acquire) &&
        expected != requested) {
        qFatal("Service endpoint policy cannot change after initialization");
    }
}

bool isDebugHttpTransportEnabled() {
    return gTransportMode.load(std::memory_order_acquire) == kDebugHttpTransport;
}

} // namespace OpenMeeting
