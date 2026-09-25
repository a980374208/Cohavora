# The application and window fixtures need identical production UI objects.
# OBJECT targets retain the previous direct-object link semantics while compiling
# each implementation only once.
function(cohavora_configure_meeting_ui_objects target)
    target_include_directories(${target} BEFORE PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${GEN_DIR}
        ${WEBRTC_ROOT}/include
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
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/xxHash)
    target_link_libraries(${target} PRIVATE
        cohavora_core
        cohavora::desktop_ui_runtime)
    set_target_properties(${target} PROPERTIES
        AUTOMOC ON
        FOLDER "meeting/ui")
endfunction()

add_library(cohavora_meeting_widgets OBJECT
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_dialogs.h
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_dialogs.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_log_console.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_log_console.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/audio_visualizer_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/audio_visualizer_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/participants_list_model.h
    ${PROJECT_SOURCE_DIR}/src/ui/participants_list_model.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/participant_item_delegate.h
    ${PROJECT_SOURCE_DIR}/src/ui/participant_item_delegate.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/participants_sidebar_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/participants_sidebar_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_chat_sidebar_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_chat_sidebar_widget.cpp)
cohavora_configure_meeting_ui_objects(cohavora_meeting_widgets)
target_link_libraries(cohavora_meeting_widgets PRIVATE
    cohavora_meeting_network
    cohavora_meeting_runtime)

add_library(cohavora_video_canvas_ui OBJECT
    ${PROJECT_SOURCE_DIR}/src/render/backend_module.h
    ${PROJECT_SOURCE_DIR}/src/render/backend_module.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/render/module_video_canvas.h
    ${PROJECT_SOURCE_DIR}/src/ui/render/module_video_canvas.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/render/gl_video_canvas.h
    ${PROJECT_SOURCE_DIR}/src/ui/render/gl_video_canvas.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/render/video_canvas.h
    ${PROJECT_SOURCE_DIR}/src/ui/render/video_canvas.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/render/video_canvas_factory.cpp)
cohavora_configure_meeting_ui_objects(cohavora_video_canvas_ui)
target_include_directories(cohavora_video_canvas_ui PRIVATE
    ${TDESKTOP_LIBS_DIR}/tg_angle/include)
target_link_libraries(cohavora_video_canvas_ui PRIVATE
    cohavora_qt_video_render
    ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(cohavora_video_canvas_ui PRIVATE d3d11 dxgi)
endif()

add_library(cohavora_meeting_window_ui OBJECT
    ${PROJECT_SOURCE_DIR}/src/ui/camera_switch_completion_owner.h
    ${PROJECT_SOURCE_DIR}/src/ui/camera_switch_completion_owner.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_room_window.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_room_window.cpp)
cohavora_configure_meeting_ui_objects(cohavora_meeting_window_ui)
target_link_libraries(cohavora_meeting_window_ui PRIVATE
    cohavora_meeting_network
    cohavora_meeting_runtime
    cohavora_qt_video_render
    cohavora_whiteboard_ui)
