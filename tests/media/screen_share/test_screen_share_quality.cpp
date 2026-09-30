#include "src/media/screen_share_quality.h"
#include "tests/runtime/probes/screen_capture_fallback_test.h"
#include "tests/support/test_check.h"
#include <iostream>

int main() {
    using namespace livekit;
    using namespace std::chrono_literals;
    const ScreenShareQuality defaults;
    TEST_CHECK(defaults.fps == 20 && defaults.resolution == ScreenShareResolution::Auto);
    struct Case { int w, h, out_w, out_h; };
    for (const auto value : {Case{1366,768,1366,768}, {1920,1080,1920,1080},
            {2560,1440,2560,1440}, {3840,2160,2560,1440}, {2560,1600,2304,1440},
            {3440,1440,2560,1070}, {1440,2560,1440,2560}, {2160,3840,1440,2560},
            {1365,767,1364,766}, {7680,4320,2560,1440}}) {
        const auto output = ResolveScreenShareProfile(value.w, value.h, defaults, 42);
        TEST_CHECK(output && output->width == value.out_w && output->height == value.out_h);
        TEST_CHECK(output->source_width == value.w && output->source_height == value.h && output->revision == 42);
    }
    for (const int fps : {15,20,30}) {
        for (const auto mode : {ScreenShareResolution::P720, ScreenShareResolution::P1080,
                ScreenShareResolution::P1440, ScreenShareResolution::Native}) {
            const auto small = ResolveScreenShareProfile(640,480,{mode,fps});
            TEST_CHECK(small && small->width == 640 && small->height == 480);
        }
    }
    TEST_CHECK(ResolveScreenShareProfile(3840,2160,{ScreenShareResolution::Native,30})->width == 3840);
    TEST_CHECK(!ResolveScreenShareProfile(7680,4320,{ScreenShareResolution::Native,30}));
    TEST_CHECK(!ResolveScreenShareProfile(1,1080,defaults));
    TEST_CHECK(!ResolveScreenShareProfile(1920,0,defaults));
    TEST_CHECK(!ResolveScreenShareProfile(1920,1080,{ScreenShareResolution::Auto,0}));
    TEST_CHECK(!ResolveScreenShareProfile(1920,1080,{ScreenShareResolution(99),20}));
    ScreenCaptureDeadline deadline;
    const auto start = ScreenCaptureDeadline::Clock::time_point{};
    deadline.Reset(start,20);
    TEST_CHECK(deadline.Advance(start+12ms) == start+50ms);
    TEST_CHECK(deadline.Advance(start+62ms) == start+100ms);
    TEST_CHECK(deadline.Advance(start+220ms) == start+270ms);
    deadline.Reset(start+225ms,30);
    TEST_CHECK(deadline.Advance(start+230ms) == start+225ms+33333333ns);
    TestScreenCaptureFallback();
    std::cout << "screen share policy, deadlines and live fallback FPS PASS\n";
}
