#pragma once

namespace livekit::platform {

// Ensures the Windows PE TLS callback calling OPENSSL_thread_stop() on
// DLL_THREAD_DETACH is linked into the executable when OpenSSL is statically
// linked. No-op on non-Windows platforms.
#if defined(_WIN32)
void EnsureOpenSslThreadCleanupRegistered() noexcept;
#else
inline void EnsureOpenSslThreadCleanupRegistered() noexcept {}
#endif

} // namespace livekit::platform
