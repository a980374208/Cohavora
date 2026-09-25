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
    ${PROJECT_SOURCE_DIR}/src/ui/audio_device_test_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/audio_device_test_controller.h
    ${PROJECT_SOURCE_DIR}/src/ui/camera_preview_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/camera_preview_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_ui_integration.h
    ${PROJECT_SOURCE_DIR}/src/core/whiteboard/whiteboard_transport.h
)

target_include_directories(cohavora_app BEFORE PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${WEBRTC_ROOT}/include/third_party/libyuv/include)

target_link_libraries(cohavora_app PRIVATE
    cohavora_dual_tls_link_compat
    cohavora::desktop_ui_runtime
    cohavora_meeting_widgets
    cohavora_video_canvas_ui
    cohavora_meeting_window_ui
    cohavora_meeting_network
    cohavora_meeting_runtime
    cohavora_qt_video_render
    cohavora_whiteboard_ui
    cohavora_ui_theme
    cohavora_ui_translations
    cohavora_core
    version)

set_target_properties(cohavora_app PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME "Cohavora")
source_group("Generated Files" REGULAR_EXPRESSION ".*mocs_compilation.*")
target_compile_definitions(cohavora_app PRIVATE
    COHAVORA_VERSION="${PROJECT_VERSION}")
if(WIN32)
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
target_compile_definitions(cohavora_app PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    CRL_USE_WINAPI
    ASIO_STANDALONE
    WEBRTC_WIN)

livekit_deploy_renderer(cohavora_app
    GROUP app
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
