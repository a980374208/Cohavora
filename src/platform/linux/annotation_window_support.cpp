#include "src/platform/annotation_window_support.h"

namespace MeetingUI::Platform {
void ApplyAnnotationWindowMode(WId, bool) {}
bool HandleAnnotationNativeEvent(void *, long *, bool) { return false; }
}
