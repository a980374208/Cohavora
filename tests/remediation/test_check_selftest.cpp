#include "tests/support/test_check.h"

#include <cstring>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    if (std::strcmp(argv[1], "--fail") == 0) {
        TEST_CHECK(false);
        // A disabled check would reach this successful exit. The parent must
        // reject it rather than treating an arbitrary child failure as proof.
        return 0;
    }
    if (std::strcmp(argv[1], "--pass") != 0) {
        return 2;
    }

    int evaluations = 0;
    TEST_CHECK(++evaluations == 1);
    // Deliberately independent of TEST_CHECK: catches disabled or repeated
    // evaluation even if the helper under test is itself broken.
    if (evaluations != 1) {
        return 3;
    }
    TEST_CHECK(true);
    std::puts("CHECK_PASS_SIDE_EFFECTS=1");
    return 0;
}
