#pragma once

// Set by the target's public build contract, never by a fixture source file.
#ifndef COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
#define COHAVORA_ENABLE_MEDIA_DIAGNOSTICS 0
#endif
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS != 0 && COHAVORA_ENABLE_MEDIA_DIAGNOSTICS != 1
#error COHAVORA_ENABLE_MEDIA_DIAGNOSTICS must be 0 or 1
#endif
#if defined(_MSC_VER)
#if COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
#pragma detect_mismatch("cohavora_media_diagnostics", "1")
#else
#pragma detect_mismatch("cohavora_media_diagnostics", "0")
#endif
#endif
