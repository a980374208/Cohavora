#include <cmath>
#include <iostream>

#include "src/ui/dx11/dx11_color_conversion.h"

namespace {

bool NearlyEqual(float actual, float expected, float epsilon = 0.0002f) {
    return std::fabs(actual - expected) <= epsilon;
}

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[TestDx11ColorConversion] " << message << std::endl;
        return false;
    }
    return true;
}

} // namespace

int main() {
    using namespace livekit::render;
    using livekit::dx11::MakeYuvColorConversion;

    bool passed = true;
    const auto default_conversion = MakeYuvColorConversion({});
    passed &= Check(NearlyEqual(default_conversion.yuv_to_rgb[0][0], 1.164383f),
                    "default must be BT.601 limited range");
    passed &= Check(NearlyEqual(default_conversion.yuv_to_rgb[2][1], 2.017232f),
                    "BT.601 limited U coefficient mismatch");
    passed &= Check(NearlyEqual(default_conversion.yuv_offset[0], -16.0f / 255.0f),
                    "limited range Y offset mismatch");

    const auto full_709 = MakeYuvColorConversion({RenderColorMatrix::Bt709, RenderColorRange::Full});
    passed &= Check(NearlyEqual(full_709.yuv_to_rgb[0][0], 1.0f),
                    "full range must not apply limited Y scaling");
    passed &= Check(NearlyEqual(full_709.yuv_to_rgb[0][2], 1.574800f),
                    "BT.709 full V coefficient mismatch");
    passed &= Check(NearlyEqual(full_709.yuv_offset[0], 0.0f),
                    "full range Y offset must be zero");

    const auto limited_2020 = MakeYuvColorConversion({RenderColorMatrix::Bt2020, RenderColorRange::Limited});
    passed &= Check(NearlyEqual(limited_2020.yuv_to_rgb[0][2], 1.678674f),
                    "BT.2020 limited V coefficient mismatch");
    passed &= Check(NearlyEqual(limited_2020.yuv_to_rgb[2][1], 2.141772f),
                    "BT.2020 limited U coefficient mismatch");

    if (!passed) {
        return 1;
    }
    std::cout << "[TestDx11ColorConversion] matrix/range policy passed" << std::endl;
    return 0;
}
