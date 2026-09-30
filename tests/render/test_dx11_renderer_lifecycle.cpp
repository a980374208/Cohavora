#include <cstdint>
#include <iostream>

#include "src/ui/dx11/dx11_renderer.h"

int main() {
    using livekit::dx11::Dx11Renderer;

    // The test hook fails after parameter validation but before any platform
    // D3D call. This is deterministic on CI, RDP, and machines without a GPU.
    Dx11Renderer::SetForceInitializationFailureForTesting(true);
    Dx11Renderer renderer;
    const auto testWindow = reinterpret_cast<HWND>(static_cast<uintptr_t>(1));
    for (int i = 0; i < 16; ++i) {
        if (renderer.Initialize(testWindow, 64, 64)) {
            std::cerr << "[TestDx11RendererLifecycle] forced initialization unexpectedly succeeded" << std::endl;
            return 1;
        }
        if (renderer.is_initialized()) {
            std::cerr << "[TestDx11RendererLifecycle] renderer remained partially initialized" << std::endl;
            return 1;
        }
        renderer.Cleanup();
        renderer.Cleanup();
    }
    Dx11Renderer::SetForceInitializationFailureForTesting(false);

    // Invalid input must also be repeatable and leave no resources behind.
    if (renderer.Initialize(nullptr, 64, 64)) {
        std::cerr << "[TestDx11RendererLifecycle] null HWND unexpectedly succeeded" << std::endl;
        return 1;
    }
    renderer.Cleanup();
    renderer.Cleanup();

    std::cout << "[TestDx11RendererLifecycle] forced failure and repeated cleanup passed" << std::endl;
    return 0;
}
