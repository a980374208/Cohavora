add_library(cohavora_meeting_network STATIC
    ${PROJECT_SOURCE_DIR}/src/net/http_types.h
    ${PROJECT_SOURCE_DIR}/src/net/meeting_types.h
    ${PROJECT_SOURCE_DIR}/src/net/meeting_types.cpp
    ${PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.h
    ${PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.cpp
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
endif()
set_target_properties(cohavora_meeting_network PROPERTIES
    AUTOMOC ON
    FOLDER "meeting")

add_library(cohavora_remote_control STATIC
    ${PROJECT_SOURCE_DIR}/src/core/remote_control/remote_control.h
    ${PROJECT_SOURCE_DIR}/src/core/remote_control/remote_control.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/win/remote_input.h
    ${PROJECT_SOURCE_DIR}/src/platform/win/remote_input.cpp)
target_include_directories(cohavora_remote_control PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(cohavora_remote_control PRIVATE nlohmann_json::nlohmann_json user32)
target_compile_definitions(cohavora_remote_control PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(cohavora_remote_control PROPERTIES AUTOMOC OFF FOLDER "meeting")

add_library(cohavora_meeting_runtime STATIC
    ${PROJECT_SOURCE_DIR}/src/core/remote_control/meeting_remote_control.cpp
    ${PROJECT_SOURCE_DIR}/src/core/meeting_coordinator.h
    ${PROJECT_SOURCE_DIR}/src/core/meeting_coordinator.cpp)

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
