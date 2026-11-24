#include "../openssl_thread_cleanup.h"

#include <openssl/crypto.h>
#include <windows.h>

namespace {

// OpenSSL 3's DLL build cleans up per-thread DRBG / sparse-array / error state
// in crypto/dllmain.c on DLL_THREAD_DETACH. When OpenSSL is statically linked
// into Cohavora (x64-windows-static), no libcrypto DllMain runs; short-lived
// worker threads (such as QNetworkAccessManager HTTP threads) otherwise leak
// their thread-local OpenSSL state on exit.
//
// Mirror OpenSSL's own DllMain contract strictly: act ONLY on DLL_THREAD_DETACH
// (never on DLL_PROCESS_DETACH or attach). OPENSSL_thread_stop() is a fast
// no-op before OpenSSL initializes, after OPENSSL_cleanup() runs, and on
// threads that never touched OpenSSL.
void NTAPI OnOpenSslTlsCallback(PVOID /*dllHandle*/, DWORD reason, PVOID /*reserved*/) {
    if (reason == DLL_THREAD_DETACH) {
        OPENSSL_thread_stop();
    }
}

} // namespace

#if defined(_MSC_VER)
#if defined(_M_AMD64) || defined(_M_ARM64)
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:cohavora_openssl_tls_callback")
#pragma const_seg(".CRT$XLB")
extern "C" {
extern const PIMAGE_TLS_CALLBACK cohavora_openssl_tls_callback = OnOpenSslTlsCallback;
}
#pragma const_seg()
#else
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_cohavora_openssl_tls_callback")
#pragma data_seg(".CRT$XLB")
extern "C" {
extern PIMAGE_TLS_CALLBACK cohavora_openssl_tls_callback = OnOpenSslTlsCallback;
}
#pragma data_seg()
#endif
#endif

namespace livekit::platform {

void EnsureOpenSslThreadCleanupRegistered() noexcept {
#if defined(_MSC_VER)
    volatile const PIMAGE_TLS_CALLBACK anchor = cohavora_openssl_tls_callback;
    (void)anchor;
#endif
}

} // namespace livekit::platform
