add_executable(cohavora_app
    main_meeting_app.cpp
    debug_login_options.cpp
    debug_login_options.h
    ${PROJECT_SOURCE_DIR}/src/net/http_types.h
    ${PROJECT_SOURCE_DIR}/src/net/meeting_types.h
    ${PROJECT_SOURCE_DIR}/src/net/meeting_types.cpp
    ${PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.h
    ${PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.cpp
    ${PROJECT_SOURCE_DIR}/src/net/session_manager.h
    ${PROJECT_SOURCE_DIR}/src/net/session_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/core/meeting_catalog_controller.h
    ${PROJECT_SOURCE_DIR}/src/core/meeting_catalog_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/net/credential_store.cpp
    ${PROJECT_SOURCE_DIR}/src/net/credential_store.h
    ${PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.cpp
    ${PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.h
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
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_dialogs.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_dialogs.h
    ${PROJECT_SOURCE_DIR}/src/ui/settings_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/settings_dialog.h
    ${PROJECT_SOURCE_DIR}/src/ui/audio_device_test_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/audio_device_test_controller.h
    ${PROJECT_SOURCE_DIR}/src/ui/camera_preview_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/camera_preview_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_room_window.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_room_window.h
    ${PROJECT_SOURCE_DIR}/src/ui/camera_switch_completion_owner.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/camera_switch_completion_owner.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_log_console.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_log_console.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_ui_integration.h
    ${PROJECT_SOURCE_DIR}/src/core/meeting_coordinator.h
    ${PROJECT_SOURCE_DIR}/src/core/meeting_coordinator.cpp
    ${PROJECT_SOURCE_DIR}/src/core/whiteboard/whiteboard_transport.h
    ${PROJECT_SOURCE_DIR}/src/render/qt_cpu_video_renderer.h
    ${PROJECT_SOURCE_DIR}/src/render/qt_cpu_video_renderer.cpp
    ${PROJECT_SOURCE_DIR}/src/render/video_render_session.h
    ${PROJECT_SOURCE_DIR}/src/render/video_render_session.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/audio_visualizer_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/audio_visualizer_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/participants_list_model.h
    ${PROJECT_SOURCE_DIR}/src/ui/participants_list_model.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/participant_item_delegate.h
    ${PROJECT_SOURCE_DIR}/src/ui/participant_item_delegate.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/participants_sidebar_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/participants_sidebar_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_chat_sidebar_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_chat_sidebar_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/render/backend_module.h
    ${PROJECT_SOURCE_DIR}/src/render/backend_module.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/render/module_video_canvas.h
    ${PROJECT_SOURCE_DIR}/src/ui/render/module_video_canvas.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/render/gl_video_canvas.h
    ${PROJECT_SOURCE_DIR}/src/ui/render/gl_video_canvas.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/render/video_canvas.h
    ${PROJECT_SOURCE_DIR}/src/ui/render/video_canvas.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/render/video_canvas_factory.cpp)

target_include_directories(cohavora_app BEFORE PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${WEBRTC_ROOT}/include/third_party/libyuv/include
    ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_ui
    ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/gen
    ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/gen/styles
    ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_base
    ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_rpl
    ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_crl
    ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/GSL/include
    ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/range-v3/include
    ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/expected/include
    ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/xxHash
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore/5.15.18/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui/5.15.18/QtGui
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtWidgets
    ${TDESKTOP_LIBS_DIR}/zlib/include)

target_link_libraries(cohavora_app PRIVATE
    cohavora_whiteboard_ui
    cohavora_ui_theme
    cohavora_ui_translations
    ${CMAKE_DL_LIBS}
    desktop_lib_ui
    desktop_lib_base
    desktop_lib_crl
    desktop_lz4
    ${TDESKTOP_LIBS_DIR}/tg_angle/out/Release/tg_angle.lib
    ${TDESKTOP_LIBS_DIR}/zlib/Release/zlibstatic.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Gui.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Widgets.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Network.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Svg.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5FontDatabaseSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5EventDispatcherSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5ThemeSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5WindowsUIAutomationSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5AccessibilitySupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5DeviceDiscoverySupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5EdidSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtfreetype.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtharfbuzz.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtlibpng.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/platforms/qwindows.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/styles/qwindowsvistastyle.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qsvg.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qico.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qjpeg.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qgif.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qtga.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qtiff.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qwbmp.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qwebp.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/iconengines/qsvgicon.lib
    ${TDESKTOP_LIBS_DIR}/mozjpeg/release/jpeg-static.lib
    cohavora_core
    Dwmapi
    UxTheme
    d3d9
    dxgi
    d3d11
    d3dcompiler
    dxguid
    netapi32
    userenv
    imm32
    winmm
    wtsapi32
    version)

set_target_properties(cohavora_app PROPERTIES
    OUTPUT_NAME "Cohavora"
    MSVC_RUNTIME_LIBRARY "MultiThreaded")
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
    WEBRTC_WIN
    NDEBUG
    _ITERATOR_DEBUG_LEVEL=0
    $<$<CONFIG:Debug>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
    $<$<CONFIG:Debug>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>)
target_link_options(cohavora_app PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>)

livekit_deploy_renderer(cohavora_app)
