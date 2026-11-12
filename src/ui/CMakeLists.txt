cohavora_group_qt_generated_sources()

set(LIVEKIT_PROJECT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(CMAKE_AUTOMOC ON)

if(UNIX AND NOT APPLE AND NOT COHAVORA_LINUX_PRODUCT_UI)
    qt5_add_resources(cohavora_ui_theme_resources app_theme.qrc)
    add_library(cohavora_ui_theme OBJECT
        app_theme.cpp app_theme.h linux_system_theme.cpp linux_system_theme.h
        ${cohavora_ui_theme_resources})
    target_include_directories(cohavora_ui_theme PUBLIC "${LIVEKIT_PROJECT_SOURCE_DIR}")
    target_link_libraries(cohavora_ui_theme PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets Qt5::DBus)
    qt5_import_plugins(cohavora_ui_theme
        EXCLUDE_BY_TYPE egldeviceintegrations
        EXCLUDE_BY_TYPE iconengines
        EXCLUDE_BY_TYPE imageformats
        EXCLUDE_BY_TYPE platforms
        EXCLUDE_BY_TYPE xcbglintegrations)
    return()
endif()

add_subdirectory(tdesktop)
foreach(livekit_desktop_target IN ITEMS
        desktop_lz4
        desktop_lib_crl
        desktop_lib_base
        desktop_lib_ui)
    set_property(TARGET ${livekit_desktop_target} PROPERTY FOLDER "desktop")
endforeach()

qt5_add_resources(cohavora_ui_theme_resources app_theme.qrc)
add_library(cohavora_ui_theme OBJECT
    app_icons.cpp
    app_icons.h
    app_theme.cpp
    app_theme.h
    ${cohavora_ui_theme_resources})
target_include_directories(cohavora_ui_theme PUBLIC "${LIVEKIT_PROJECT_SOURCE_DIR}")
target_link_libraries(cohavora_ui_theme PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets)
set_target_properties(cohavora_ui_theme PROPERTIES AUTOMOC OFF FOLDER "resources")
if(UNIX AND NOT APPLE)
    # The Linux development sysroot may supply headers without system-wide
    # -dev packages, as it already does for the screencast dependencies.
    find_path(COHAVORA_WAYLAND_INCLUDE_DIR wayland-client-core.h
        HINTS ${COHAVORA_SCREENCAST_INCLUDE_DIRS} REQUIRED)
    find_package(Qt5XkbCommonSupport ${COHAVORA_QT_VERSION} EXACT CONFIG REQUIRED
        PATHS "${QT_ROOT}/lib/cmake/Qt5XkbCommonSupport" NO_DEFAULT_PATH)
    target_sources(cohavora_ui_theme PRIVATE
        linux_system_theme.cpp linux_system_theme.h
        linux_window_frame.cpp linux_window_frame.h
        linux_xcb_window_shadow.cpp linux_xcb_window_shadow.h)
    target_include_directories(cohavora_ui_theme SYSTEM PRIVATE ${COHAVORA_WAYLAND_INCLUDE_DIR})
    # The product pins Qt 5.15.18. Qt 5 has no public subsurface stacking API.
    target_link_libraries(cohavora_ui_theme PRIVATE Qt5::DBus
        Qt5::GuiPrivate Qt5::WaylandClientPrivate Qt5::XkbCommonSupportPrivate X11::xcb X11::xcb_shape)
    set_target_properties(cohavora_ui_theme PROPERTIES AUTOMOC ON)
endif()

include("${LIVEKIT_PROJECT_SOURCE_DIR}/cmake/UiTranslations.cmake")
livekit_add_ui_translations(cohavora_ui_translations
    SOURCE_ROOT "${LIVEKIT_PROJECT_SOURCE_DIR}")

# The document is value-only; Qt rendering and interaction have a separate owner.
add_library(cohavora_whiteboard_model STATIC
    ../core/whiteboard/whiteboard_asset.cpp
    ../core/whiteboard/whiteboard_asset.h
    ../core/whiteboard/whiteboard_document.cpp
    ../core/whiteboard/whiteboard_document.h
    ../core/whiteboard/whiteboard_protocol.cpp
    ../core/whiteboard/whiteboard_protocol.h
    ../core/whiteboard/whiteboard_runtime.cpp
    ../core/whiteboard/whiteboard_runtime.h)
target_include_directories(cohavora_whiteboard_model PUBLIC "${LIVEKIT_PROJECT_SOURCE_DIR}")
target_link_libraries(cohavora_whiteboard_model PRIVATE
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json)
set_target_properties(cohavora_whiteboard_model PROPERTIES AUTOMOC OFF FOLDER "whiteboard")

add_library(cohavora_whiteboard_ui STATIC
    whiteboard/accessible_combo_box.cpp whiteboard/accessible_combo_box.h
    whiteboard/whiteboard_image_loader.cpp whiteboard/whiteboard_image_loader.h
    whiteboard/whiteboard_renderer.cpp whiteboard/whiteboard_renderer.h
    whiteboard/whiteboard_canvas.cpp whiteboard/whiteboard_canvas.h
    whiteboard/whiteboard_panel.cpp whiteboard/whiteboard_panel.h
    whiteboard/annotation_overlay_window.cpp whiteboard/annotation_overlay_window.h
    ../media/screen_binding.h
    ../platform/annotation_window_support.h)
if(WIN32)
    target_sources(cohavora_whiteboard_ui PRIVATE ../platform/win/annotation_window_support.cpp)
else()
    target_sources(cohavora_whiteboard_ui PRIVATE ../platform/linux/annotation_window_support.cpp)
endif()
target_include_directories(cohavora_whiteboard_ui PUBLIC "${LIVEKIT_PROJECT_SOURCE_DIR}")
target_link_libraries(cohavora_whiteboard_ui PUBLIC cohavora_whiteboard_model Qt5::Core Qt5::Gui Qt5::Widgets)
set_target_properties(cohavora_whiteboard_ui PROPERTIES AUTOMOC ON FOLDER "whiteboard")
if(MSVC)
    foreach(target IN ITEMS cohavora_whiteboard_model cohavora_whiteboard_ui)
        target_compile_options(${target} PRIVATE /utf-8)
    endforeach()
endif()
