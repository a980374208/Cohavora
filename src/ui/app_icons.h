#pragma once

#include <QtGui/QIcon>

namespace MeetingUI::AppTheme {
enum class Icon { Close, Details, Video, Audio, Information };
// Theme icons render at the requested size, without image-format discovery or
// decoding every standard-icon resolution during window construction.
QIcon icon(Icon kind);
} // namespace MeetingUI::AppTheme
