# Qt 5 Linux toolkit adapters

The product wrapper builds the pinned Desktop App Toolkit submodules without
editing their sources. Linux selects CRL's Qt scheduler and Linux clock, POSIX
file locks, and the toolkit's Qt 5 compatible XCB/platform implementations.
Windows keeps its existing WinAPI sources and libraries.

The current upstream Linux implementation additionally expects GI C++ generated
portal bindings and some Qt 6 private APIs. This directory supplies the small
subset needed by Cohavora's shared Qt 5 UI:

- `qt5_platform.cpp`: executable/resource paths, file operations and asynchronous
  FileManager1 folder reveal, with Qt Desktop Services as a fallback.
- `qt5_screen_reader.cpp`: Qt accessibility activation on Wayland; asynchronous
  `org.a11y.Status.ScreenReaderEnabled` observation on X11. Pending replies are
  rejected after a newer status event or service restart.
- `qt5_window_title.cpp`: XSettings and asynchronous Settings portal observation
  of GNOME's title-button layout. The shared toolkit still renders the buttons.
- `include/qpa/qplatformwindow_p.h`: forwards the Qt 6 private include name to
  Qt 5's platform-window header. Qt 6 native Wayland API blocks remain disabled;
  Qt 5 provides normal window move/resize/transient-parent behavior itself.

This does not implement the later screen-capture or remote-control portal
sessions. Qt 6-only foreign Wayland parent import, custom compositor margins and
native window-menu extensions are unavailable through the toolkit on Qt 5.
The unused upstream MPRIS, URL registration, haptic and inhibition GI adapters
are not built; callers must add a real platform implementation before using
those interfaces. No success-returning stubs are provided for them.

The build uses the selected static Qt's private CMake targets and bundled JPEG
headers/archives so their ABI stays aligned. `gio-2.0` development metadata is
required by the upstream battery-saving implementation; `QT_NO_KEYWORDS` is
private to the base target to avoid GLib's `signals` field colliding with Qt.
