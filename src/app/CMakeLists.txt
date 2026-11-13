cohavora_group_qt_generated_sources()

if(UNIX AND NOT APPLE AND NOT COHAVORA_LINUX_PRODUCT_UI)
    add_executable(cohavora_app
        linux_minimal_main.cpp
        ${PROJECT_SOURCE_DIR}/src/media/linux_camera_capture.cpp
        ${PROJECT_SOURCE_DIR}/src/media/linux_camera_capture.h)
    target_include_directories(cohavora_app BEFORE PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${WEBRTC_ROOT}/include/third_party/libyuv/include)
    target_link_libraries(cohavora_app PRIVATE
        cohavora_core
        cohavora_qt_video_render
        cohavora_ui_theme
        cohavora::qt_application_runtime)
    target_compile_definitions(cohavora_app PRIVATE
        COHAVORA_VERSION="${PROJECT_VERSION}")
    set_target_properties(cohavora_app PROPERTIES
        AUTOMOC ON OUTPUT_NAME "Cohavora")
    qt5_import_plugins(cohavora_app
        INCLUDE_BY_TYPE platforms
            Qt5::QXcbIntegrationPlugin Qt5::QOffscreenIntegrationPlugin
        INCLUDE_BY_TYPE imageformats
            Qt5::QGifPlugin Qt5::QICOPlugin
            Qt5::QWebpPlugin Qt5::QSvgPlugin
        INCLUDE_BY_TYPE iconengines Qt5::QSvgIconPlugin)
    return()
endif()

include(${PROJECT_SOURCE_DIR}/cmake/CohavoraUpdates.cmake)

add_executable(cohavora_app
    main_meeting_app.cpp
    debug_login_options.cpp
    debug_login_options.h
    ${PROJECT_SOURCE_DIR}/src/core/meeting_catalog_controller.h
    ${PROJECT_SOURCE_DIR}/src/core/meeting_catalog_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/login_dialog.h
    ${PROJECT_SOURCE_DIR}/src/ui/login_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/sidebar_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/sidebar_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/action_card_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/action_card_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_list_model.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_list_model.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_list_delegate.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_list_delegate.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_list_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_list_dialog.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_booking_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_booking_dialog.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_detail_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_detail_dialog.h
    ${PROJECT_SOURCE_DIR}/src/ui/schedule_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/schedule_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_main_window.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_main_window.h
    ${PROJECT_SOURCE_DIR}/src/ui/settings_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/settings_dialog.h
    ${PROJECT_SOURCE_DIR}/src/ui/audio_device_test_controller.h
    ${PROJECT_SOURCE_DIR}/src/ui/camera_preview_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/camera_preview_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_ui_integration.h
    ${PROJECT_SOURCE_DIR}/src/core/whiteboard/whiteboard_transport.h
)

if(WIN32)
    target_sources(cohavora_app PRIVATE ${PROJECT_SOURCE_DIR}/src/ui/audio_device_test_controller.cpp)
else()
    target_sources(cohavora_app PRIVATE ${PROJECT_SOURCE_DIR}/src/ui/audio_device_test_controller_linux.cpp)
endif()

target_include_directories(cohavora_app BEFORE PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${WEBRTC_ROOT}/include/third_party/libyuv/include)

target_link_libraries(cohavora_app PRIVATE
    cohavora::desktop_ui_runtime
    cohavora_meeting_widgets
    cohavora_video_canvas_ui
    cohavora_meeting_window_ui
    cohavora_device_discovery
    cohavora_meeting_network
    cohavora_meeting_runtime
    cohavora_qt_video_render
    cohavora_whiteboard_ui
    cohavora_ui_theme
    cohavora_ui_translations
    cohavora_updates
    cohavora_core)
if(WIN32)
    target_link_libraries(cohavora_app PRIVATE version)
endif()

set_target_properties(cohavora_app PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME "Cohavora")
target_compile_definitions(cohavora_app PRIVATE
    COHAVORA_VERSION="${PROJECT_VERSION}")
if(WIN32)
    set_target_properties(cohavora_app PROPERTIES WIN32_EXECUTABLE TRUE)
    enable_language(RC)
    configure_file(cohavora.rc.in "${CMAKE_CURRENT_BINARY_DIR}/cohavora.rc" @ONLY)
    target_sources(cohavora_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/cohavora.rc")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/cohavora.rc" PROPERTIES
        OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/src/ui/icons/cohavora.ico")

    set(cohavora_resource_files "${CMAKE_CURRENT_BINARY_DIR}/cohavora.rc")
    get_target_property(nlohmann_json_interface_sources
        nlohmann_json::nlohmann_json INTERFACE_SOURCES)
    foreach(interface_source IN LISTS nlohmann_json_interface_sources)
        if(interface_source MATCHES "\\.natvis$")
            list(APPEND cohavora_resource_files "${interface_source}")
        endif()
    endforeach()
    source_group("Resources" FILES ${cohavora_resource_files})
endif()
if(WIN32)
target_compile_definitions(cohavora_app PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    CRL_USE_WINAPI
    ASIO_STANDALONE
    WEBRTC_WIN)
else()
    target_link_libraries(cohavora_app PRIVATE Qt5::WaylandClient)
    qt5_import_plugins(cohavora_app
        INCLUDE_BY_TYPE platforms Qt5::QWaylandIntegrationPlugin Qt5::QXcbIntegrationPlugin Qt5::QOffscreenIntegrationPlugin
        INCLUDE_BY_TYPE wayland-shell-integration Qt5::QWaylandXdgShellIntegrationPlugin
        INCLUDE_BY_TYPE wayland-decoration-client Qt5::QWaylandBradientDecorationPlugin
        INCLUDE_BY_TYPE wayland-graphics-integration-client Qt5::QWaylandEglClientBufferPlugin
        INCLUDE_BY_TYPE imageformats Qt5::QJpegPlugin Qt5::QGifPlugin Qt5::QICOPlugin Qt5::QWebpPlugin Qt5::QSvgPlugin
        INCLUDE_BY_TYPE iconengines Qt5::QSvgIconPlugin)
endif()

livekit_deploy_renderer(cohavora_app
    GROUP app
    DESTINATION "$<IF:$<BOOL:${CMAKE_CONFIGURATION_TYPES}>,${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>,${CMAKE_CURRENT_BINARY_DIR}>")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND COHAVORA_WEBRTC_HAS_LINUX_VAAPI)
    # Only the reviewed private runtime is deployed. Its FFmpeg symbols remain
    # inside the DSO; the application never links another public libavcodec.
    set_property(TARGET cohavora_app APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    set_property(TARGET cohavora_app APPEND PROPERTY LINK_DEPENDS
        "${COHAVORA_WEBRTC_LINUX_VAAPI_RUNTIME}")
    add_custom_command(TARGET cohavora_app POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${COHAVORA_WEBRTC_LINUX_VAAPI_RUNTIME}"
            "$<TARGET_FILE_DIR:cohavora_app>/libcohavora_vaapi_runtime.so.1"
        VERBATIM)
endif()
