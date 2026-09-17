#pragma once

#include <cstdio>
#include <cstdlib>

namespace livekit::test {

// Unlike assert(), this check and its expression remain active under NDEBUG.
[[noreturn]] inline void FailCheck(const char* expression, const char* file, int line) {
    std::fprintf(stderr, "TEST_CHECK failed: %s (%s:%d)\n", expression, file, line);
    std::fflush(stderr);
    std::exit(EXIT_FAILURE);
}

} // namespace livekit::test

#define TEST_CHECK(expression)                                                   \
    do {                                                                         \
        if (!(expression)) {                                                     \
            ::livekit::test::FailCheck(#expression, __FILE__, __LINE__);            \
        }                                                                        \
    } while (false)
