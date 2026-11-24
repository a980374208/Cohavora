#pragma once

#include <QtGui/QIcon>

namespace MeetingUI::AppTheme {
enum class ColorRole;
enum class Icon {
    Close, Details, Video, VideoOff, Audio, Information,
    LayoutGrid, LayoutRight, LayoutTop, LayoutUpperL, LayoutLowerL, LayoutFloating, ChevronDown, ChevronRight,
    Plus, MoreHorizontal, Back, History, SidebarRight, Expand, Collapse, Settings,
    Copy, Check, Refresh, Edit, ErrorCircle, Microphone, MicrophoneOff, Send, Stop,
    Sparkle, ChatMode, Antenna, ArrowDown, Trash,
    Calendar, ScreenShare, Shield, Eye, EyeOff, Server,
};
// Theme icons render at the requested size, without image-format discovery or
// decoding every standard-icon resolution during window construction.
QIcon icon(Icon kind);
QIcon icon(Icon kind, ColorRole role);
} // namespace MeetingUI::AppTheme
