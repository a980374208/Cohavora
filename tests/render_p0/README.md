# Rendering P0

This standalone experiment fixes the v1 module contract in
`src/render/api/render_backend_api.h` and checks an isolated Qt OpenGL child
surface. It does not replace the production DX11 canvas, implement a video
renderer module, or change the meeting/session pipeline.

```powershell
cmake -S tests/render_p0 -B build-render-p0 -G "Visual Studio 18 2026" -A x64
cmake --build build-render-p0 --config Release --target test_render_abi render_surface_probe
ctest --test-dir build-render-p0 -C Release --output-on-failure
```

The bundled Windows Qt uses GLES/ANGLE and the static CRT, as does the existing
application. Its relocated dependencies are linked without modifying vendor
files. For another Qt 5.15 installation, use
`-DLIVEKIT_P0_BUNDLED_QT=OFF -DCMAKE_PREFIX_PATH=<Qt prefix>`. For a headless ABI
check only, set `-DLIVEKIT_P0_BUILD_SURFACE=OFF`. Non-Windows builds use platform
Qt packages and need their own runtime evidence.

`render_surface_probe` without arguments opens the interactive experiment.
`--self-test --output <directory>` verifies native-screen pixels and saves PNGs
and `surface-result.json`. A visible desktop is required. Tests deliberately
fail when a GPU surface cannot be verified; an unavailable GPU is not a PASS.
For a distinct DPI configuration, run with `QT_SCALE_FACTOR=1.5` and a different
output directory. Do not rerun an unchanged successful gate.

The experiment uses QOpenGLWindow + createWindowContainer, keeping the parent
QWidget window raster. The host uploads a video stand-in and a separate
premultiplied RGBA recovery/Pin overlay to the same GL surface. A raised sibling
QWidget is observed but is not the overlay strategy. Pixel checks cover actual
native composition, resize, hide/show and raster fallback without rebuilding
the parent. Input checks send synthetic Qt events, not physical mouse input.
Failure is injected during initialization and inside paint; teardown is queued
until paint returns. The generation guard prevents queued work reviving a
retired surface. Test-only framebuffer readback is not a display path.

The child requests RGBA8 explicitly; the default EGL format need not have eight
bits per channel. This format must not be set globally on ordinary raster
windows. Overlays are rasterized in logical coordinates with their image DPR
set, so text remains the same size across CPU and GL. On resize, the probe queues
an update after native surface resizing, rather than waiting for another video
frame. A QWidget top-level may report RasterGLSurface even before GL exists;
fallback acceptance therefore uses actual CPU pixels and stable window identity,
not a strict RasterSurface enum comparison.

The ABI fixture is a real C shared library consumed dynamically by C++. It is
explicitly not a graphics backend. It verifies symbol/calling-convention and
POD layout compatibility, bounded version negotiation, opaque allocation and
module-owned destruction. Production selection/loading policy belongs to P2.

Real driver/context loss, cross-monitor DPI, target-platform window managers,
long-running meetings, I420/NV12 shaders, and production module integration
remain outside P0 evidence. Initialization-failure injection occurs after Qt
has created a context; it must not be described as proof of a missing GL driver.
