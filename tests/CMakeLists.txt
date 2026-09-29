# Test targets are isolated from production targets and grouped by CMake-capable IDEs.
cohavora_group_qt_generated_sources()

set(LIVEKIT_PROJECT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(LIVEKIT_TEST_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(CMAKE_FOLDER "tests")

option(LIVEKIT_BUILD_EXTENDED_TESTS
    "Build optional feature tests (APM, permissions, VAD, E2EE)"
    OFF)
option(LIVEKIT_BUILD_HARDWARE_TESTS
    "Build tests that enumerate or open local WASAPI/DirectShow devices"
    OFF)
option(LIVEKIT_BUILD_EXTERNAL_TESTS
    "Build tests that connect to an external LiveKit server"
    OFF)
option(LIVEKIT_BUILD_RENDERER_RUNTIME_TESTS
    "Register renderer tests that require an interactive desktop or real GPU"
    OFF)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/TestSupport.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/NativeTests.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/SecurityTests.cmake)

if(COHAVORA_BUILD_QT_TESTS)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/MeetingTests.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/RenderTests.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/ApplicationTests.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/WhiteboardTests.cmake)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/UiaTests.cmake)
endif()

set_property(TEST diagnostic_pipeline_test camera_owner_remediation_test
    camera_switch_transaction_test local_unpublish_transaction_test
    unpublish_lifetime_remediation_test APPEND PROPERTY LABELS LOGGING_LG3)
if(COHAVORA_BUILD_QT_TESTS)
    set_property(TEST openmeeting_http_test http_owner_remediation_test
        participant_snapshot_remediation_test meeting_startup_transaction_test
        meeting_subscription_telemetry_reconnect_test
        APPEND PROPERTY LABELS LOGGING_LG3)
    set_property(TEST participant_snapshot_remediation_test
        APPEND PROPERTY LABELS LOGGING_LG6)
endif()

# A wedged test must not consume the enclosing CI job indefinitely. Individual
# stress, hardware, and external tests override this budget where necessary.
livekit_apply_default_test_timeout(60)

# Keep test binaries and module fixtures in the established top-level output
# directory even though their target definitions now live in this subdirectory.
get_property(LIVEKIT_TEST_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(livekit_test_target IN LISTS LIVEKIT_TEST_TARGETS)
    set_property(TARGET ${livekit_test_target} PROPERTY FOLDER "tests")
    get_target_property(livekit_test_target_type ${livekit_test_target} TYPE)
    if(NOT livekit_test_target_type STREQUAL "INTERFACE_LIBRARY")
        set_target_properties(${livekit_test_target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
    endif()
endforeach()
