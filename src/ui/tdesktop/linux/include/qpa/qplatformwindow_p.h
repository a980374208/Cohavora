#pragma once

// The toolkit includes this Qt 6 header before its feature-guarded Wayland
// implementation. Qt 5 exposes the common window API in qplatformwindow.h.
#include <qpa/qplatformwindow.h>
