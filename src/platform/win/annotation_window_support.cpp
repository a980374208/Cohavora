#include "annotation_window_support.h"

#include <windows.h>

namespace MeetingUI::Platform {
void ApplyAnnotationWindowMode(WId window, bool passThrough) {
    const auto hwnd = reinterpret_cast<HWND>(window);
    if (!hwnd) return;
    auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    style |= WS_EX_LAYERED | WS_EX_TOOLWINDOW;
    if (passThrough) style |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    else style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style);
    EnableWindow(hwnd, passThrough ? FALSE : TRUE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

bool HandleAnnotationNativeEvent(void *message, long *result, bool passThrough) {
    const auto native = static_cast<MSG *>(message);
    if (!passThrough || !native || native->message != WM_NCHITTEST) return false;
    *result = HTTRANSPARENT;
    return true;
}
}
