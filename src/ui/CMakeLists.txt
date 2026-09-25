set(LIVEKIT_PROJECT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(CMAKE_AUTOMOC ON)

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
    app_theme.cpp
    app_theme.h
    ${cohavora_ui_theme_resources})
target_include_directories(cohavora_ui_theme PUBLIC "${LIVEKIT_PROJECT_SOURCE_DIR}")
target_link_libraries(cohavora_ui_theme PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets)
set_target_properties(cohavora_ui_theme PROPERTIES AUTOMOC OFF FOLDER "resources")

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
    whiteboard/whiteboard_image_loader.cpp whiteboard/whiteboard_image_loader.h
    whiteboard/whiteboard_renderer.cpp whiteboard/whiteboard_renderer.h
    whiteboard/whiteboard_canvas.cpp whiteboard/whiteboard_canvas.h
    whiteboard/whiteboard_panel.cpp whiteboard/whiteboard_panel.h
    whiteboard/annotation_overlay_window.cpp whiteboard/annotation_overlay_window.h
    ../media/screen_binding.h
    ../platform/win/annotation_window_support.cpp ../platform/win/annotation_window_support.h)
target_include_directories(cohavora_whiteboard_ui PUBLIC "${LIVEKIT_PROJECT_SOURCE_DIR}")
target_link_libraries(cohavora_whiteboard_ui PUBLIC cohavora_whiteboard_model Qt5::Core Qt5::Gui Qt5::Widgets)
set_target_properties(cohavora_whiteboard_ui PROPERTIES AUTOMOC ON FOLDER "whiteboard")
if(MSVC)
    foreach(target IN ITEMS cohavora_whiteboard_model cohavora_whiteboard_ui)
        target_compile_options(${target} PRIVATE /utf-8)
    endforeach()
endif()
