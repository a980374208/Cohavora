#include <iostream>

#include "src/ui/dx11/dx11_shaders.h"

namespace {

bool Compile(const char* name, const char* source, const char* target) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    if (!livekit::dx11::CompileShader(source, "main", target, blob.GetAddressOf())) {
        std::cerr << "[TestDx11Shaders] failed to compile " << name << std::endl;
        return false;
    }
    return true;
}

} // namespace

int main() {
    using namespace livekit::dx11;
    const bool passed =
        Compile("vertex", kVertexShaderSource, "vs_5_0") &&
        Compile("i420", kPixelShaderI420Source, "ps_5_0") &&
        Compile("nv12", kPixelShaderNV12Source, "ps_5_0") &&
        Compile("rgba", kPixelShaderRGBASource, "ps_5_0") &&
        Compile("solid_color", kPixelShaderSolidColorSource, "ps_5_0");
    if (!passed) {
        return 1;
    }
    std::cout << "[TestDx11Shaders] all embedded shader sources compiled" << std::endl;
    return 0;
}
