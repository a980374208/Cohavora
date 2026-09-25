# OpenMeeting HTTP Client Test
add_executable(test_openmeeting_http
    ${LIVEKIT_TEST_SOURCE_DIR}/test_openmeeting_http.cpp
)
target_include_directories(test_openmeeting_http BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${GEN_DIR})
target_link_libraries(test_openmeeting_http PRIVATE
    cohavora_dual_tls_link_compat
    cohavora_meeting_network
    cohavora_core
    cohavora::qt_network_runtime)

target_compile_definitions(test_openmeeting_http PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS)
add_test(NAME openmeeting_http_test COMMAND test_openmeeting_http --debug)
add_test(NAME openmeeting_http_watchdog_test
    COMMAND ${CMAKE_COMMAND}
        "-DTEST_EXECUTABLE=$<TARGET_FILE:test_openmeeting_http>"
        -P "${LIVEKIT_TEST_SOURCE_DIR}/remediation/verify_http_watchdog.cmake"
)

# CPPQT-001: the real Coordinator callbacks run against a controllable
# admission backend, while the Room boundary is intercepted before SDK/device
# creation. The production target continues to use the default HTTP backend.
add_executable(test_http_admission_owner
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_http_admission_owner.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/meeting_log_test_sink.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_http_admission_owner BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${GEN_DIR})
target_link_libraries(test_http_admission_owner PRIVATE
    cohavora_meeting_runtime
    cohavora_core
    cohavora_whiteboard_model
    cohavora::qt_widgets_runtime)
target_compile_definitions(test_http_admission_owner PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    ASIO_STANDALONE
    WEBRTC_WIN)
add_test(NAME http_owner_remediation_test COMMAND test_http_admission_owner --debug)
set_tests_properties(http_owner_remediation_test PROPERTIES TIMEOUT 60)

# Coordinator session runtime serialization test
add_executable(test_meeting_session_runtime
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_meeting_session_runtime.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/meeting_session_runtime.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/publication_catalog.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/publication_catalog.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/video_demand_policy.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/video_demand_policy.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/video_demand_types.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/telemetry/session_telemetry.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/telemetry/session_telemetry.h
)
target_include_directories(test_meeting_session_runtime BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/rtc
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/rpc
    ${WEBRTC_ROOT}/include
)
target_link_libraries(test_meeting_session_runtime PRIVATE
    asio::asio
    ole32
    cohavora::qt_core_runtime)
target_compile_definitions(test_meeting_session_runtime PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    ASIO_STANDALONE)
if(MSVC)
    target_compile_options(test_meeting_session_runtime PRIVATE /utf-8)
endif()
add_test(NAME meeting_session_runtime_test COMMAND test_meeting_session_runtime)
set_tests_properties(meeting_session_runtime_test PROPERTIES
    TIMEOUT 60
    LABELS "TELEMETRY_S1;CORE_REGRESSION")

# IDA2-P0-001: immutable participant snapshots, incarnation tickets, ordered
# Room delivery, and delayed Qt projection rejection for retired instances.
add_executable(test_participant_snapshot_remediation
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_participant_snapshot_remediation.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_participant_snapshot_remediation BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${GEN_DIR})
target_link_libraries(test_participant_snapshot_remediation PRIVATE
    cohavora_meeting_runtime
    cohavora_core
    cohavora_whiteboard_model
    cohavora::qt_widgets_runtime)
target_compile_definitions(test_participant_snapshot_remediation PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    ASIO_STANDALONE
    WEBRTC_WIN)
add_test(NAME participant_snapshot_remediation_test
    COMMAND test_participant_snapshot_remediation)
set_tests_properties(participant_snapshot_remediation_test PROPERTIES TIMEOUT 60)

# IDA2 window acceptance uses the same allowed test source with a distinct
# QApplication branch. It links the production console exactly once; the core
# target keeps its original synchronous LogToConsole hook and owner cases.
add_executable(test_participant_window_remediation
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_participant_snapshot_remediation.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_participant_window_remediation BEFORE PRIVATE
    ${WEBRTC_ROOT}/include/third_party/perfetto/include
    ${WEBRTC_ROOT}/include/out-x64-release/gen/third_party/perfetto
    ${WEBRTC_ROOT}/include/out-x64-release/gen/third_party/perfetto/build_config
)
livekit_configure_qt_test(test_participant_window_remediation)
target_link_libraries(test_participant_window_remediation PRIVATE
    cohavora_meeting_widgets
    cohavora_video_canvas_ui
    cohavora_meeting_window_ui
    cohavora_meeting_runtime
    cohavora_qt_video_render
    cohavora_ui_translations)
set_target_properties(test_participant_window_remediation PROPERTIES AUTOMOC OFF)
target_compile_definitions(test_participant_window_remediation PRIVATE IDA2_WINDOW_ACCEPTANCE)
# Entry ownership and capture/publication source identity use synthetic media;
# keep this regression in the default suite without enabling device/GPU tests.
add_test(NAME meeting_entry_media_contract_test
    COMMAND test_participant_window_remediation --entry-media-contract)
set_tests_properties(meeting_entry_media_contract_test PROPERTIES
    TIMEOUT 30 LABELS "CORE_REGRESSION")
add_test(NAME meeting_audio_preferences_test
    COMMAND test_participant_window_remediation --audio-preferences-contract)
set_tests_properties(meeting_audio_preferences_test PROPERTIES
    TIMEOUT 30 LABELS "CORE_REGRESSION")
add_test(NAME meeting_local_media_state_test
    COMMAND test_participant_window_remediation --local-media-state)
set_tests_properties(meeting_local_media_state_test PROPERTIES
    TIMEOUT 30 LABELS "CORE_REGRESSION")
add_test(NAME meeting_telemetry_ui_test
    COMMAND test_participant_window_remediation --telemetry-ui)
set_tests_properties(meeting_telemetry_ui_test PROPERTIES
    TIMEOUT 30 LABELS "TELEMETRY_S1;TELEMETRY_S7")
add_test(NAME meeting_telemetry_s7_ui_100_test
    COMMAND test_participant_window_remediation --telemetry-s7-acceptance --scale-100)
add_test(NAME meeting_telemetry_s7_ui_150_test
    COMMAND test_participant_window_remediation --telemetry-s7-acceptance --scale-150)
add_test(NAME meeting_telemetry_s7_ui_200_test
    COMMAND test_participant_window_remediation --telemetry-s7-acceptance --scale-200)
set_tests_properties(
    meeting_telemetry_s7_ui_100_test
    meeting_telemetry_s7_ui_150_test
    meeting_telemetry_s7_ui_200_test
    PROPERTIES
        TIMEOUT 30
        RUN_SERIAL TRUE
        LABELS "TELEMETRY_S7_ACCEPTANCE")
add_test(NAME meeting_subscription_telemetry_reconnect_test
    COMMAND test_participant_window_remediation --subscription-telemetry-reconnect)
set_tests_properties(meeting_subscription_telemetry_reconnect_test PROPERTIES
    TIMEOUT 30 LABELS "TELEMETRY_S2;CORE_REGRESSION")
add_test(NAME meeting_remote_media_plan_test
    COMMAND test_participant_window_remediation --phase-c-room)
set_tests_properties(meeting_remote_media_plan_test PROPERTIES
    TIMEOUT 30 LABELS "CORE_REGRESSION")
add_test(NAME meeting_video_viewport_render_lease_test
    COMMAND test_participant_window_remediation --phase-d-window)
set_tests_properties(meeting_video_viewport_render_lease_test PROPERTIES
    TIMEOUT 60 LABELS "CORE_REGRESSION")
add_test(NAME meeting_moderation_contract_test
    COMMAND test_participant_window_remediation --moderation-contract)
set_tests_properties(meeting_moderation_contract_test PROPERTIES
    TIMEOUT 30 LABELS "CORE_REGRESSION")
if(LIVEKIT_BUILD_RENDERER_RUNTIME_TESTS)
    add_test(NAME participant_window_remediation_test COMMAND test_participant_window_remediation)
    set_tests_properties(participant_window_remediation_test PROPERTIES
        TIMEOUT 60 LABELS "CORE_REGRESSION;RENDER_RUNTIME")
endif()

# Coordinator startup transaction test: a meeting is committed only after both
# required local tracks publish, otherwise it must roll back to failure.
add_executable(test_meeting_startup_transaction
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_meeting_startup_transaction.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/meeting_startup_transaction.h
)
target_include_directories(test_meeting_startup_transaction BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
)
target_compile_definitions(test_meeting_startup_transaction PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
add_test(NAME meeting_startup_transaction_test COMMAND test_meeting_startup_transaction)
