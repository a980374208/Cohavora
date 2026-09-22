#pragma once

#include <QtGui/qwindowdefs.h>

namespace MeetingUI::Platform {
void ApplyAnnotationWindowMode(WId window, bool passThrough);
bool HandleAnnotationNativeEvent(void *message, long *result, bool passThrough);
}
