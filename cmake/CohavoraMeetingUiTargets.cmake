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
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
endfunction()

add_library(cohavora_device_discovery STATIC
    ${PROJECT_SOURCE_DIR}/src/ui/cached_device_discovery.h
    ${PROJECT_SOURCE_DIR}/src/ui/camera_device_discovery.h
    ${PROJECT_SOURCE_DIR}/src/ui/camera_device_discovery.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/audio_device_discovery.h
    ${PROJECT_SOURCE_DIR}/src/ui/audio_device_discovery.cpp)
target_include_directories(cohavora_device_discovery PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(cohavora_device_discovery PRIVATE cohavora_core Qt5::Core)
set_target_properties(cohavora_device_discovery PROPERTIES AUTOMOC OFF FOLDER "meeting/ui")

add_library(cohavora_meeting_widgets OBJECT
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_panel_model.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_panel_controller.h
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_panel_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_chart_widgets.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_live_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_dialogs.h
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_dialogs.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/telemetry_details_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_log_console.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_log_console.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/diagnostic_qt_bridge.h
    ${PROJECT_SOURCE_DIR}/src/ui/diagnostic_qt_bridge.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/audio_visualizer_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/audio_visualizer_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/participants_list_model.h
    ${PROJECT_SOURCE_DIR}/src/ui/participants_list_model.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/participant_item_delegate.h
    ${PROJECT_SOURCE_DIR}/src/ui/participant_item_delegate.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/participants_sidebar_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/participants_sidebar_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_chat_sidebar_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_chat_sidebar_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/copilot_markdown_parser.h
    ${PROJECT_SOURCE_DIR}/src/ui/copilot_markdown_parser.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/copilot_chat_canvas_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/copilot_chat_canvas_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/copilot_sidebar_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/copilot_sidebar_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/initials_avatar_painter.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/initials_avatar_painter.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/org_tree_model.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/org_tree_model.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contact_list_model.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contact_list_model.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contact_item_delegate.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contact_item_delegate.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contact_request_list_model.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contact_request_list_model.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contact_request_item_delegate.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contact_request_item_delegate.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contacts_tree_model.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contacts_tree_model.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contacts_tree_delegate.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contacts_tree_delegate.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contact_picker_dialog.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contact_picker_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/org_history_list_model.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/org_history_list_model.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/org_history_item_delegate.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/org_history_item_delegate.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/user_profile_dialog.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/user_profile_dialog.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contacts_page_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/contacts/contacts_page_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/call/incoming_call_window.h
    ${PROJECT_SOURCE_DIR}/src/ui/call/incoming_call_window.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/call/outgoing_call_overlay.h
    ${PROJECT_SOURCE_DIR}/src/ui/call/outgoing_call_overlay.cpp)
cohavora_configure_meeting_ui_objects(cohavora_meeting_widgets)
target_link_libraries(cohavora_meeting_widgets PRIVATE
    cohavora_device_discovery
    cohavora_meeting_network
    cohavora_meeting_runtime
    cohavora_agent)

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
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_layout_controls.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_layout_controls.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/render/share_view_transform.h
    ${PROJECT_SOURCE_DIR}/src/ui/floating_member_list_window.h
    ${PROJECT_SOURCE_DIR}/src/ui/floating_member_list_window.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/shared_screen_window.h
    ${PROJECT_SOURCE_DIR}/src/ui/shared_screen_window.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/remote_control_ui.h
    ${PROJECT_SOURCE_DIR}/src/ui/remote_control_ui.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/remote_control_key_mapping.h
    ${PROJECT_SOURCE_DIR}/src/ui/remote_control_key_mapping.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/camera_switch_completion_owner.h
    ${PROJECT_SOURCE_DIR}/src/ui/camera_switch_completion_owner.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/detail/meeting_room_helpers.h
    ${PROJECT_SOURCE_DIR}/src/ui/video_tile_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/video_tile_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/room_top_bar_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/room_top_bar_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/room_bottom_bar_widget.h
    ${PROJECT_SOURCE_DIR}/src/ui/room_bottom_bar_widget.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_room_window.h
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_room_window.cpp
    ${PROJECT_SOURCE_DIR}/src/ui/meeting_room_window_devices.cpp)
cohavora_configure_meeting_ui_objects(cohavora_meeting_window_ui)
target_compile_definitions(cohavora_meeting_window_ui PRIVATE
    COHAVORA_SHOW_CONSOLE_BUTTON=$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>,1,0>)
target_link_libraries(cohavora_meeting_window_ui PRIVATE
    cohavora_device_discovery
    cohavora_meeting_network
    cohavora_meeting_runtime
    cohavora_agent
    cohavora_meeting_widgets
    cohavora_qt_video_render
    cohavora_whiteboard_ui)
