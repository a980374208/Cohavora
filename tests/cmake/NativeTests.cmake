# Unit/Integration Tests
add_executable(livekit_signaling_tests_run_v4
    ${LIVEKIT_TEST_SOURCE_DIR}/main.cpp
)

target_link_libraries(livekit_signaling_tests_run_v4 PRIVATE
    livekit_signaling
)

add_executable(test_signaling_url_policy
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_signaling_url_policy.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_signaling_url_policy PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_signaling_url_policy PRIVATE livekit_signaling)
set_target_properties(test_signaling_url_policy PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

# Panic Guard Test
add_executable(test_panic_guard
    ${LIVEKIT_TEST_SOURCE_DIR}/test_panic_guard.cpp
)
target_include_directories(test_panic_guard PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_panic_guard PRIVATE
    livekit_signaling
)

# Advanced Communication Test
add_executable(test_advanced_communication
    ${LIVEKIT_TEST_SOURCE_DIR}/test_advanced_communication.cpp
)
target_include_directories(test_advanced_communication PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_advanced_communication PRIVATE
    livekit_signaling
)

# Chunking & Simulcast Test
add_executable(test_chunking_simulcast
    ${LIVEKIT_TEST_SOURCE_DIR}/test_chunking_simulcast.cpp
)
target_include_directories(test_chunking_simulcast PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_chunking_simulcast PRIVATE
    livekit_signaling
)

# Inbound DataStream assembler unit test
add_executable(test_data_stream_assembler
    ${LIVEKIT_TEST_SOURCE_DIR}/test_data_stream_assembler.cpp
)
target_include_directories(test_data_stream_assembler PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_data_stream_assembler PRIVATE
    livekit_signaling
)

if(LIVEKIT_BUILD_EXTENDED_TESTS)
    add_executable(test_apm_3a
        ${LIVEKIT_TEST_SOURCE_DIR}/test_apm_3a.cpp
    )
    target_include_directories(test_apm_3a PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_apm_3a PRIVATE
        livekit_signaling
    )

    add_executable(test_attributes_permissions_quality
        ${LIVEKIT_TEST_SOURCE_DIR}/test_attributes_permissions_quality.cpp
    )
    target_include_directories(test_attributes_permissions_quality PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_attributes_permissions_quality PRIVATE
        livekit_signaling
    )
endif()

# RPC System Test
add_executable(test_rpc_system
    ${LIVEKIT_TEST_SOURCE_DIR}/test_rpc_system.cpp
)
target_include_directories(test_rpc_system PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_rpc_system PRIVATE
    livekit_signaling
)

# Media Streaming Test
add_executable(test_media_streaming
    ${LIVEKIT_TEST_SOURCE_DIR}/test_media_streaming.cpp
)
target_include_directories(test_media_streaming PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_media_streaming PRIVATE
    livekit_signaling
)

# Stats System Test
add_executable(test_stats_system
    ${LIVEKIT_TEST_SOURCE_DIR}/test_stats_system.cpp
)
target_include_directories(test_stats_system PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_stats_system PRIVATE
    livekit_signaling
)

# Server room-state delta contract: room metadata, connection quality, stream
# state, and subscription permissions must commit before listener delivery.
add_executable(test_room_state_events
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_room_state_events.cpp
)
target_include_directories(test_room_state_events PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_room_state_events PRIVATE
    livekit_signaling
)

# Canonical remote-publication ownership, control dispatch, and stale-session
# rejection. This exercises the P1 lifecycle independently of live media.
add_executable(test_remote_publication_lifecycle
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_remote_publication_lifecycle.cpp
)
target_include_directories(test_remote_publication_lifecycle PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_remote_publication_lifecycle PRIVATE
    livekit_signaling
)

# Local media unpublish is committed by publisher SDP answer, not by a guessed
# SignalRequest. This locks down the protocol gate and public API validation.
add_executable(test_local_unpublish_transaction
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_local_unpublish_transaction.cpp
)
target_include_directories(test_local_unpublish_transaction PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_local_unpublish_transaction PRIVATE
    livekit_signaling
)

# NEW-CPP_QT-002: real Room transaction with privately controlled async boundaries.
add_executable(test_unpublish_lifetime
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_unpublish_lifetime.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_unpublish_lifetime PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_unpublish_lifetime PRIVATE livekit_signaling)

# Active camera source hot-switch transaction with first-frame verification and rollback
add_executable(test_camera_switch_transaction
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_camera_switch_transaction.cpp
)
target_link_libraries(test_camera_switch_transaction PRIVATE
    livekit_signaling
)

# AEC playout render reference integration with framing and reset lifecycle
add_executable(test_audio_render_reference_apm
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_audio_render_reference_apm.cpp
)
target_link_libraries(test_audio_render_reference_apm PRIVATE
    livekit_signaling
)

# Meeting recovery UX and connection state loopback contract
add_executable(test_meeting_recovery_ux
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_meeting_recovery_ux.cpp
)
target_link_libraries(test_meeting_recovery_ux PRIVATE
    livekit_signaling
)

# Modern data stream messaging contract
add_executable(test_data_stream_messaging
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_data_stream_messaging.cpp
)
target_link_libraries(test_data_stream_messaging PRIVATE
    livekit_signaling
)

# NEW-CPP_QT-001: actual writer/factory lifetime and packet contracts. Checks stay
# active with the application's inherited NDEBUG and dependency ABI settings.
add_executable(test_stream_writer_lifetime
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_stream_writer_lifetime.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_stream_writer_lifetime PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_stream_writer_lifetime PRIVATE livekit_signaling)

# NEW-SIGNALING-002: stream failure terminal state, transport admission, and
# native-session isolation through soft resume and full restart.
add_executable(test_stream_delivery_remediation
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_stream_delivery_remediation.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_stream_delivery_remediation PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_stream_delivery_remediation PRIVATE livekit_signaling)

if(LIVEKIT_BUILD_EXTERNAL_TESTS)
    add_executable(test_livekit_official_connect
        ${LIVEKIT_TEST_SOURCE_DIR}/test_livekit_official_connect.cpp
    )
    target_link_libraries(test_livekit_official_connect PRIVATE
        livekit_signaling
    )

    # Manual L3 harness. It is intentionally not registered with CTest because
    # it requires two real-service participants and operator-controlled faults.
    add_executable(test_stream_delivery_runtime
        ${LIVEKIT_TEST_SOURCE_DIR}/runtime/test_stream_delivery_runtime.cpp
    )
    target_link_libraries(test_stream_delivery_runtime PRIVATE
        livekit_signaling
    )
endif()

if(LIVEKIT_BUILD_HARDWARE_TESTS)
    add_executable(test_wasapi_capture
        ${LIVEKIT_TEST_SOURCE_DIR}/test_wasapi_capture.cpp
    )
    target_include_directories(test_wasapi_capture PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_wasapi_capture PRIVATE
        livekit_signaling
    )

    add_executable(test_dshow_capture
        ${LIVEKIT_TEST_SOURCE_DIR}/test_dshow_capture.cpp
    )
    target_include_directories(test_dshow_capture PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_dshow_capture PRIVATE
        livekit_signaling
    )
endif()

# First-audible-frame playout warmup unit test
add_executable(test_audio_playout_warmup
    ${LIVEKIT_TEST_SOURCE_DIR}/test_audio_playout_warmup.cpp
)
target_include_directories(test_audio_playout_warmup PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_audio_playout_warmup PRIVATE
    livekit_signaling
)

# Screen-share lifecycle regression and opt-in real Windows capture probe.
add_executable(test_screen_share_session ${LIVEKIT_TEST_SOURCE_DIR}/test_screen_share_session.cpp)
target_include_directories(test_screen_share_session PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_screen_share_session PRIVATE livekit_signaling)
add_test(NAME screen_share_session_test COMMAND test_screen_share_session)
set_tests_properties(screen_share_session_test PROPERTIES TIMEOUT 30 LABELS "CORE_REGRESSION")
add_executable(test_desktop_capture_runtime ${LIVEKIT_TEST_SOURCE_DIR}/runtime/test_desktop_capture_runtime.cpp)
target_include_directories(test_desktop_capture_runtime PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_desktop_capture_runtime PRIVATE livekit_signaling)
add_executable(test_screen_share_runtime ${LIVEKIT_TEST_SOURCE_DIR}/runtime/test_screen_share_runtime.cpp)
target_include_directories(test_screen_share_runtime PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_screen_share_runtime PRIVATE livekit_signaling)
# Opt-in desktop interaction; deliberately not part of device-free CTest.

# VP8 Simulcast Test
add_executable(test_simulcast
    ${LIVEKIT_TEST_SOURCE_DIR}/test_simulcast.cpp
)
target_include_directories(test_simulcast PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_simulcast PRIVATE
    livekit_signaling
)

if(LIVEKIT_BUILD_EXTENDED_TESTS)
    add_executable(test_adaptive_stream
        ${LIVEKIT_TEST_SOURCE_DIR}/test_adaptive_stream.cpp
    )
    target_include_directories(test_adaptive_stream PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_adaptive_stream PRIVATE
        livekit_signaling
    )

    add_executable(test_speaker_vad
        ${LIVEKIT_TEST_SOURCE_DIR}/test_speaker_vad.cpp
    )
    target_include_directories(test_speaker_vad PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_speaker_vad PRIVATE
        livekit_signaling
    )

    add_executable(test_e2ee
        ${LIVEKIT_TEST_SOURCE_DIR}/test_e2ee.cpp
    )
    target_include_directories(test_e2ee PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_e2ee PRIVATE
        livekit_signaling
        ${OPENSSL_LIBRARIES}
    )
endif()

# Lifecycle & Reconnect Stress Test (GAP-02)
add_executable(test_stress_lifecycle
    ${LIVEKIT_TEST_SOURCE_DIR}/test_stress_lifecycle.cpp
)
target_link_libraries(test_stress_lifecycle PRIVATE
    livekit_signaling
)

# Simulate Scenario Suite (Matching Flutter sendSimulateScenario)
add_executable(test_simulate_scenario
    ${LIVEKIT_TEST_SOURCE_DIR}/test_simulate_scenario.cpp
)
target_link_libraries(test_simulate_scenario PRIVATE
    livekit_signaling
)

# Multi-Codec Simulcast & Backup Codecs Test (GAP-03)
add_executable(test_backup_codecs
    ${LIVEKIT_TEST_SOURCE_DIR}/test_backup_codecs.cpp
)
target_include_directories(test_backup_codecs PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_backup_codecs PRIVATE
    livekit_signaling
)

# Multi-User Grid Math & Avatar Hash Test
add_executable(test_meeting_ui_grid
    ${LIVEKIT_TEST_SOURCE_DIR}/test_meeting_ui_grid.cpp
)
target_include_directories(test_meeting_ui_grid PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
