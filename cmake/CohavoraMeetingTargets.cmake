add_library(cohavora_meeting_network STATIC
    ${PROJECT_SOURCE_DIR}/src/net/http_types.h
    ${PROJECT_SOURCE_DIR}/src/net/meeting_types.h
    ${PROJECT_SOURCE_DIR}/src/net/meeting_types.cpp
    ${PROJECT_SOURCE_DIR}/src/net/contact_types.h
    ${PROJECT_SOURCE_DIR}/src/net/contact_types.cpp
    ${PROJECT_SOURCE_DIR}/src/net/call_types.h
    ${PROJECT_SOURCE_DIR}/src/net/call_types.cpp
    ${PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.h
    ${PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.cpp
    ${PROJECT_SOURCE_DIR}/src/net/user_event_stream_client.h
    ${PROJECT_SOURCE_DIR}/src/net/user_event_stream_client.cpp
    ${PROJECT_SOURCE_DIR}/src/net/session_manager.h
    ${PROJECT_SOURCE_DIR}/src/net/session_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/net/credential_store.h
    ${PROJECT_SOURCE_DIR}/src/net/credential_store.cpp
    ${PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.h
    ${PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.cpp)

target_include_directories(cohavora_meeting_network PUBLIC
    ${PROJECT_SOURCE_DIR})
target_link_libraries(cohavora_meeting_network PUBLIC
    cohavora_core
    cohavora::qt_network_runtime
    OpenSSL::SSL
    OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(cohavora_meeting_network PRIVATE crypt32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(COHAVORA_SECRET_SERVICE REQUIRED IMPORTED_TARGET libsecret-1)
    target_sources(cohavora_meeting_network PRIVATE
        ${PROJECT_SOURCE_DIR}/src/net/linux_secret_service.cpp
        ${PROJECT_SOURCE_DIR}/src/net/linux_secret_service.h
        ${PROJECT_SOURCE_DIR}/src/net/linux_secret_service_store.cpp)
    target_link_libraries(cohavora_meeting_network PRIVATE PkgConfig::COHAVORA_SECRET_SERVICE)
endif()
set_target_properties(cohavora_meeting_network PROPERTIES
    AUTOMOC ON
    FOLDER "meeting")

add_library(cohavora_remote_control STATIC
    ${PROJECT_SOURCE_DIR}/src/core/remote_control/remote_control.h
    ${PROJECT_SOURCE_DIR}/src/core/remote_control/remote_control.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/remote_input.cpp)
if(WIN32)
    target_sources(cohavora_remote_control PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/win/remote_input.cpp)
    target_link_libraries(cohavora_remote_control PRIVATE user32)
endif()
target_include_directories(cohavora_remote_control PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(cohavora_remote_control PRIVATE nlohmann_json::nlohmann_json)
if(WIN32)
    target_compile_definitions(cohavora_remote_control PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
set_target_properties(cohavora_remote_control PROPERTIES AUTOMOC OFF FOLDER "meeting")

add_library(cohavora_meeting_runtime STATIC
    ${PROJECT_SOURCE_DIR}/src/core/remote_control/meeting_remote_control.cpp
    ${PROJECT_SOURCE_DIR}/src/core/meeting_catalog_controller.h
    ${PROJECT_SOURCE_DIR}/src/core/meeting_catalog_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/core/meeting_coordinator.h
    ${PROJECT_SOURCE_DIR}/src/core/meeting_coordinator.cpp
    ${PROJECT_SOURCE_DIR}/src/core/contact_snapshot_store.h
    ${PROJECT_SOURCE_DIR}/src/core/contact_snapshot_store.cpp
    ${PROJECT_SOURCE_DIR}/src/core/contact_catalog_controller.h
    ${PROJECT_SOURCE_DIR}/src/core/contact_catalog_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/core/presence_watch_controller.h
    ${PROJECT_SOURCE_DIR}/src/core/presence_watch_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/core/call_invite_controller.h
    ${PROJECT_SOURCE_DIR}/src/core/call_invite_controller.cpp
    ${PROJECT_SOURCE_DIR}/src/media/ringtone_player.h)
if(WIN32)
    target_sources(cohavora_meeting_runtime PRIVATE
        ${PROJECT_SOURCE_DIR}/src/media/ringtone_player_win.cpp)
else()
    target_sources(cohavora_meeting_runtime PRIVATE
        ${PROJECT_SOURCE_DIR}/src/media/ringtone_player_linux.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(COHAVORA_PULSE_SIMPLE QUIET IMPORTED_TARGET libpulse-simple)
            if(COHAVORA_PULSE_SIMPLE_FOUND)
                target_compile_definitions(cohavora_meeting_runtime PRIVATE COHAVORA_HAS_PULSE_SIMPLE=1)
                target_link_libraries(cohavora_meeting_runtime PRIVATE PkgConfig::COHAVORA_PULSE_SIMPLE)
            endif()
        endif()
    endif()
endif()

target_include_directories(cohavora_meeting_runtime PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${GEN_DIR})
target_link_libraries(cohavora_meeting_runtime PUBLIC
    cohavora_core
    cohavora_meeting_network
    cohavora_whiteboard_model
    cohavora_remote_control
    Qt5::Core)
set_target_properties(cohavora_meeting_runtime PROPERTIES
    AUTOMOC ON
    FOLDER "meeting")

if(MSVC)
    target_compile_options(cohavora_meeting_network PRIVATE /utf-8)
    target_compile_options(cohavora_meeting_runtime PRIVATE /utf-8)
endif()

add_library(cohavora_agent STATIC
    ${PROJECT_SOURCE_DIR}/src/agent/agent_types.h
    ${PROJECT_SOURCE_DIR}/src/agent/agent_secret_store.h
    ${PROJECT_SOURCE_DIR}/src/agent/agent_secret_store.cpp
    ${PROJECT_SOURCE_DIR}/src/agent/agent_session_store.h
    ${PROJECT_SOURCE_DIR}/src/agent/agent_session_store.cpp
    ${PROJECT_SOURCE_DIR}/src/agent/agent_transport.h
    ${PROJECT_SOURCE_DIR}/src/agent/openai_compatible_transport.h
    ${PROJECT_SOURCE_DIR}/src/agent/openai_compatible_transport.cpp
    ${PROJECT_SOURCE_DIR}/src/agent/meeting_tool_registry.h
    ${PROJECT_SOURCE_DIR}/src/agent/meeting_tool_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/agent/agent_coordinator.h
    ${PROJECT_SOURCE_DIR}/src/agent/agent_coordinator.cpp
    ${PROJECT_SOURCE_DIR}/src/agent/agent_speech_service.h
    ${PROJECT_SOURCE_DIR}/src/agent/agent_speech_service.cpp
    ${PROJECT_SOURCE_DIR}/src/agent/streaming_asr_session.h
    ${PROJECT_SOURCE_DIR}/src/agent/streaming_asr_session.cpp
    ${PROJECT_SOURCE_DIR}/src/agent/streaming_asr_adapters.h
    ${PROJECT_SOURCE_DIR}/src/agent/streaming_asr_adapters.cpp
    ${PROJECT_SOURCE_DIR}/src/agent/meeting_translator_service.h
    ${PROJECT_SOURCE_DIR}/src/agent/meeting_translator_service.cpp
    ${PROJECT_SOURCE_DIR}/src/agent/room_agent_publisher.h
    ${PROJECT_SOURCE_DIR}/src/agent/room_agent_publisher.cpp
    ${PROJECT_SOURCE_DIR}/src/agent/agent_ui_grounding.h
    ${PROJECT_SOURCE_DIR}/src/agent/agent_ui_grounding.cpp)

target_include_directories(cohavora_agent PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${GEN_DIR})
target_link_libraries(cohavora_agent PUBLIC
    cohavora_meeting_network
    cohavora_meeting_runtime
    cohavora_core
    cohavora::qt_network_runtime
    PRIVATE
    OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(cohavora_agent PRIVATE crypt32)
endif()
set_target_properties(cohavora_agent PROPERTIES
    AUTOMOC ON
    FOLDER "meeting")
if(MSVC)
    target_compile_options(cohavora_agent PRIVATE /utf-8)
endif()
