# CPPQT-002: real MeetingRoomWindow wiring with deterministic native camera
# completion scheduling. The fixture does not start network or physical media.
if(COHAVORA_BUILD_QT_TESTS)
add_executable(test_camera_owner_remediation
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_camera_owner_remediation.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h)
livekit_configure_qt_test(test_camera_owner_remediation)
target_link_libraries(test_camera_owner_remediation PRIVATE
    cohavora_meeting_widgets
    cohavora_video_canvas_ui
    cohavora_meeting_window_ui
    cohavora_meeting_runtime
    cohavora_qt_video_render)
set_target_properties(test_camera_owner_remediation PROPERTIES AUTOMOC OFF)
add_test(NAME camera_owner_remediation_test COMMAND test_camera_owner_remediation)
set_tests_properties(camera_owner_remediation_test PROPERTIES TIMEOUT 60)
endif()

# PR-SEC-001: typed safe summaries, production output boundary, and
# connection-handshake wire/log separation.
add_executable(test_log_redaction
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_log_redaction.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_log_redaction PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_log_redaction PRIVATE cohavora_core)
add_test(NAME log_redaction_test COMMAND test_log_redaction)
set_tests_properties(log_redaction_test PROPERTIES TIMEOUT 60)

add_executable(test_connection_log_redaction
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_connection_log_redaction.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_connection_log_redaction PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_connection_log_redaction PRIVATE cohavora_core)
add_test(NAME connection_log_redaction_test COMMAND test_connection_log_redaction)
set_tests_properties(connection_log_redaction_test PROPERTIES TIMEOUT 60)

# PR-SEC-003: real loopback TLS handshakes must authenticate the endpoint
# before either query or header credentials reach the WebSocket server.
add_executable(test_websocket_tls_verification
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_websocket_tls_verification.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_websocket_tls_verification PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_websocket_tls_verification PRIVATE cohavora_core)
add_test(NAME websocket_tls_verification_test COMMAND test_websocket_tls_verification)
set_tests_properties(websocket_tls_verification_test PROPERTIES
    TIMEOUT 60 LABELS "CORE_REGRESSION")

# Exercise the real Qt console cache without pulling unrelated production
# sources into this fixture.
if(COHAVORA_BUILD_QT_TESTS)
add_executable(test_qt_log_redaction
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_qt_log_redaction.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/meeting_log_console.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/meeting_log_console.cpp
)
livekit_configure_qt_test(test_qt_log_redaction)
add_test(NAME qt_log_redaction_test COMMAND test_qt_log_redaction)
set_tests_properties(qt_log_redaction_test PROPERTIES TIMEOUT 60)
endif()

# This verifier proves checks execute under the same inherited NDEBUG flags as
# the native application; dependency ABI definitions must remain unchanged.
add_executable(test_always_active_checks
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_check_selftest.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_always_active_checks PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
set_target_properties(test_always_active_checks PROPERTIES AUTOMOC OFF)
add_test(NAME always_active_checks_test
    COMMAND ${CMAKE_COMMAND}
        "-DTEST_EXECUTABLE=$<TARGET_FILE:test_always_active_checks>"
        -P "${LIVEKIT_TEST_SOURCE_DIR}/remediation/verify_test_check.cmake"
)

add_test(NAME signaling_tests COMMAND cohavora_core_tests)
add_test(NAME signaling_url_policy_test COMMAND test_signaling_url_policy)
set_tests_properties(signaling_url_policy_test PROPERTIES
    TIMEOUT 30)
add_test(NAME panic_guard_test COMMAND test_panic_guard)
add_test(NAME advanced_communication_test COMMAND test_advanced_communication)
add_test(NAME chunking_simulcast_test COMMAND test_chunking_simulcast)
add_test(NAME data_stream_assembler_test COMMAND test_data_stream_assembler)
add_test(NAME rpc_system_test COMMAND test_rpc_system)
add_test(NAME room_state_events_test COMMAND test_room_state_events)
add_test(NAME remote_publication_lifecycle_test COMMAND test_remote_publication_lifecycle)
add_test(NAME publication_catalog_test COMMAND test_publication_catalog)
set_tests_properties(publication_catalog_test PROPERTIES LABELS "CORE_REGRESSION")
add_test(NAME video_demand_policy_test COMMAND test_video_demand_policy)
set_tests_properties(video_demand_policy_test PROPERTIES LABELS "CORE_REGRESSION")
add_test(NAME local_unpublish_transaction_test COMMAND test_local_unpublish_transaction)
add_test(NAME unpublish_lifetime_remediation_test COMMAND test_unpublish_lifetime)
add_test(NAME camera_switch_transaction_test COMMAND test_camera_switch_transaction)
add_test(NAME audio_render_reference_apm_test COMMAND test_audio_render_reference_apm)
add_test(NAME meeting_recovery_ux_test COMMAND test_meeting_recovery_ux)
add_test(NAME data_stream_messaging_test COMMAND test_data_stream_messaging)
add_test(NAME stream_writer_lifetime_remediation_test COMMAND test_stream_writer_lifetime)
add_test(NAME stream_delivery_remediation_test COMMAND test_stream_delivery_remediation)
add_test(NAME media_streaming_test COMMAND test_media_streaming)
add_test(NAME stats_system_test COMMAND test_stats_system)
set_tests_properties(stats_system_test PROPERTIES LABELS "TELEMETRY_S1")
add_test(NAME audio_playout_warmup_test COMMAND test_audio_playout_warmup)
add_test(NAME simulcast_test COMMAND test_simulcast)
add_test(NAME stress_lifecycle_test COMMAND test_stress_lifecycle)
set_tests_properties(stress_lifecycle_test PROPERTIES TIMEOUT 120)
add_test(NAME simulate_scenario_test COMMAND test_simulate_scenario)
add_test(NAME backup_codecs_test COMMAND test_backup_codecs)
add_test(NAME meeting_ui_grid_test COMMAND test_meeting_ui_grid)

# NEW-TEST-001: repeatable, device-free assertion restoration gate. Keep the
# intentional-failure verifier alongside the tests whose checks it validates.
set_tests_properties(
    always_active_checks_test
    media_streaming_test
    audio_playout_warmup_test
    simulcast_test
    backup_codecs_test
    meeting_ui_grid_test
    PROPERTIES LABELS "ASSERTION_REGRESSION" TIMEOUT 60
)

if(LIVEKIT_BUILD_EXTENDED_TESTS)
    add_test(NAME apm_3a_test COMMAND test_apm_3a)
    add_test(NAME attributes_permissions_quality_test COMMAND test_attributes_permissions_quality)
    add_test(NAME adaptive_stream_test COMMAND test_adaptive_stream)
    add_test(NAME speaker_vad_test COMMAND test_speaker_vad)
    add_test(NAME e2ee_test COMMAND test_e2ee)
    set_tests_properties(
        apm_3a_test
        attributes_permissions_quality_test
        adaptive_stream_test
        speaker_vad_test
        e2ee_test
        PROPERTIES LABELS "ASSERTION_REGRESSION" TIMEOUT 60
    )
endif()

if(LIVEKIT_BUILD_HARDWARE_TESTS)
    add_test(NAME wasapi_capture_test COMMAND test_wasapi_capture)
endif()
if(TARGET test_dshow_capture)
    add_test(NAME dshow_capture_test COMMAND test_dshow_capture)
    set_tests_properties(dshow_capture_test PROPERTIES TIMEOUT 30 LABELS "CORE_REGRESSION")
endif()

if(LIVEKIT_BUILD_EXTERNAL_TESTS)
    add_test(NAME livekit_official_connect_test COMMAND test_livekit_official_connect)
endif()
