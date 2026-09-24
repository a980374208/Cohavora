# Unit/Integration Tests
add_executable(cohavora_core_tests
    ${LIVEKIT_TEST_SOURCE_DIR}/main.cpp
)

target_link_libraries(cohavora_core_tests PRIVATE
    cohavora_core
)

add_executable(test_signaling_url_policy
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_signaling_url_policy.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_signaling_url_policy PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_signaling_url_policy PRIVATE cohavora_core)
set_target_properties(test_signaling_url_policy PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

# Panic Guard Test
add_executable(test_panic_guard
    ${LIVEKIT_TEST_SOURCE_DIR}/test_panic_guard.cpp
)
target_include_directories(test_panic_guard PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_panic_guard PRIVATE
    cohavora_core
)

# Advanced Communication Test
add_executable(test_advanced_communication
    ${LIVEKIT_TEST_SOURCE_DIR}/test_advanced_communication.cpp
)
target_include_directories(test_advanced_communication PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_advanced_communication PRIVATE
    cohavora_core
)

# Chunking & Simulcast Test
add_executable(test_chunking_simulcast
    ${LIVEKIT_TEST_SOURCE_DIR}/test_chunking_simulcast.cpp
)
target_include_directories(test_chunking_simulcast PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_chunking_simulcast PRIVATE
    cohavora_core
)

# Inbound DataStream assembler unit test
add_executable(test_data_stream_assembler
    ${LIVEKIT_TEST_SOURCE_DIR}/test_data_stream_assembler.cpp
)
target_include_directories(test_data_stream_assembler PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_data_stream_assembler PRIVATE
    cohavora_core
)

if(LIVEKIT_BUILD_EXTENDED_TESTS)
    add_executable(test_apm_3a
        ${LIVEKIT_TEST_SOURCE_DIR}/test_apm_3a.cpp
    )
    target_include_directories(test_apm_3a PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_apm_3a PRIVATE
        cohavora_core
    )

    add_executable(test_attributes_permissions_quality
        ${LIVEKIT_TEST_SOURCE_DIR}/test_attributes_permissions_quality.cpp
    )
    target_include_directories(test_attributes_permissions_quality PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_attributes_permissions_quality PRIVATE
        cohavora_core
    )
endif()

# RPC System Test
add_executable(test_rpc_system
    ${LIVEKIT_TEST_SOURCE_DIR}/test_rpc_system.cpp
)
target_include_directories(test_rpc_system PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_rpc_system PRIVATE
    cohavora_core
)

# Media Streaming Test
add_executable(test_media_streaming
    ${LIVEKIT_TEST_SOURCE_DIR}/test_media_streaming.cpp
)
target_include_directories(test_media_streaming PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_media_streaming PRIVATE
    cohavora_core
)

# Stats System Test
add_executable(test_stats_system
    ${LIVEKIT_TEST_SOURCE_DIR}/test_stats_system.cpp
)
target_include_directories(test_stats_system PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_stats_system PRIVATE
    cohavora_core
)

add_executable(test_session_telemetry
    ${LIVEKIT_TEST_SOURCE_DIR}/test_session_telemetry.cpp
)
target_include_directories(test_session_telemetry PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_session_telemetry PRIVATE
    cohavora_core
)
add_test(NAME session_telemetry_test COMMAND test_session_telemetry)
set_tests_properties(session_telemetry_test PROPERTIES
    TIMEOUT 30
    LABELS "TELEMETRY_S1;TELEMETRY_S6;TELEMETRY_S7")

add_executable(test_stability_ledger
    ${LIVEKIT_TEST_SOURCE_DIR}/test_stability_ledger.cpp
)
target_include_directories(test_stability_ledger PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_stability_ledger PRIVATE
    cohavora_core
)
add_test(NAME stability_ledger_test COMMAND test_stability_ledger)
set_tests_properties(stability_ledger_test PROPERTIES
    TIMEOUT 30
    LABELS "TELEMETRY_S6;CORE_REGRESSION")

add_executable(test_telemetry_report
    ${LIVEKIT_TEST_SOURCE_DIR}/test_telemetry_report.cpp
)
target_include_directories(test_telemetry_report PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_telemetry_report PRIVATE cohavora_core)
add_test(NAME telemetry_report_test COMMAND test_telemetry_report)
set_tests_properties(telemetry_report_test PROPERTIES
    TIMEOUT 30
    LABELS "TELEMETRY_S7;TELEMETRY_S7_ACCEPTANCE;CORE_REGRESSION;SECURITY_REGRESSION")

# S0 telemetry capability probe: validates the packaged WebRTC stats ABI,
# endpoint codec capabilities, and the frozen renderer completion boundary.
# It is device-free and does not connect to an external LiveKit service.
add_executable(test_telemetry_capability_probe
    ${LIVEKIT_TEST_SOURCE_DIR}/test_telemetry_capability_probe.cpp
)
target_include_directories(test_telemetry_capability_probe PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_telemetry_capability_probe PRIVATE
    cohavora_core
)
add_test(NAME telemetry_capability_probe_test COMMAND test_telemetry_capability_probe)
set_tests_properties(telemetry_capability_probe_test PROPERTIES
    TIMEOUT 30
    LABELS "TELEMETRY_S0")

# S8a controlled-peer protocol, capability downgrade, and bounded clock mapping.
add_executable(test_e2e_measurement
    ${LIVEKIT_TEST_SOURCE_DIR}/test_e2e_measurement.cpp
)
target_include_directories(test_e2e_measurement PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_e2e_measurement PRIVATE cohavora_core)
add_test(NAME e2e_measurement_test COMMAND test_e2e_measurement)
set_tests_properties(e2e_measurement_test PROPERTIES
    TIMEOUT 30
    LABELS "TELEMETRY_S8A;CORE_REGRESSION")

# S8b controlled media marker and decode-to-render-submit acknowledgement gate.
add_executable(test_e2e_media_correlation
    ${LIVEKIT_TEST_SOURCE_DIR}/test_e2e_media_correlation.cpp
)
target_include_directories(test_e2e_media_correlation PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_e2e_media_correlation PRIVATE cohavora_core)
add_test(NAME e2e_media_correlation_test COMMAND test_e2e_media_correlation)
set_tests_properties(e2e_media_correlation_test PROPERTIES
    TIMEOUT 30
    LABELS "TELEMETRY_S8B;CORE_REGRESSION")

# Server room-state delta contract: room metadata, connection quality, stream
# state, and subscription permissions must commit before listener delivery.
add_executable(test_room_state_events
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_room_state_events.cpp
)
target_include_directories(test_room_state_events PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_room_state_events PRIVATE
    cohavora_core
)

# Canonical remote-publication ownership, control dispatch, and stale-session
# rejection. This exercises the P1 lifecycle independently of live media.
add_executable(test_remote_publication_lifecycle
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_remote_publication_lifecycle.cpp
)
target_include_directories(test_remote_publication_lifecycle PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_remote_publication_lifecycle PRIVATE
    cohavora_core
)

add_executable(test_publication_catalog
    ${LIVEKIT_TEST_SOURCE_DIR}/test_publication_catalog.cpp
)
target_include_directories(test_publication_catalog PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_publication_catalog PRIVATE cohavora_core)

add_executable(test_video_demand_policy
    ${LIVEKIT_TEST_SOURCE_DIR}/test_video_demand_policy.cpp
)
target_include_directories(test_video_demand_policy PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_video_demand_policy PRIVATE cohavora_core)

# Local media unpublish is committed by publisher SDP answer, not by a guessed
# SignalRequest. This locks down the protocol gate and public API validation.
add_executable(test_local_unpublish_transaction
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_local_unpublish_transaction.cpp
)
target_include_directories(test_local_unpublish_transaction PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_local_unpublish_transaction PRIVATE
    cohavora_core
)

# NEW-CPP_QT-002: real Room transaction with privately controlled async boundaries.
add_executable(test_unpublish_lifetime
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_unpublish_lifetime.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_unpublish_lifetime PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_unpublish_lifetime PRIVATE cohavora_core)

# Active camera source hot-switch transaction with first-frame verification and rollback
add_executable(test_camera_switch_transaction
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_camera_switch_transaction.cpp
)
target_link_libraries(test_camera_switch_transaction PRIVATE
    cohavora_core
)

# AEC playout render reference integration with framing and reset lifecycle
add_executable(test_audio_render_reference_apm
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_audio_render_reference_apm.cpp
)
target_link_libraries(test_audio_render_reference_apm PRIVATE
    cohavora_core
)

# Meeting recovery UX and connection state loopback contract
add_executable(test_meeting_recovery_ux
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_meeting_recovery_ux.cpp
)
target_link_libraries(test_meeting_recovery_ux PRIVATE
    cohavora_core
)

# Modern data stream messaging contract
add_executable(test_data_stream_messaging
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_data_stream_messaging.cpp
)
target_link_libraries(test_data_stream_messaging PRIVATE
    cohavora_core
)

# NEW-CPP_QT-001: actual writer/factory lifetime and packet contracts. Checks stay
# active with the application's inherited NDEBUG and dependency ABI settings.
add_executable(test_stream_writer_lifetime
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_stream_writer_lifetime.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_stream_writer_lifetime PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_stream_writer_lifetime PRIVATE cohavora_core)

# NEW-SIGNALING-002: stream failure terminal state, transport admission, and
# native-session isolation through soft resume and full restart.
add_executable(test_stream_delivery_remediation
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_stream_delivery_remediation.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
)
target_include_directories(test_stream_delivery_remediation PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_stream_delivery_remediation PRIVATE cohavora_core)

if(LIVEKIT_BUILD_EXTERNAL_TESTS)
    add_executable(test_livekit_official_connect
        ${LIVEKIT_TEST_SOURCE_DIR}/test_livekit_official_connect.cpp
    )
    target_link_libraries(test_livekit_official_connect PRIVATE
        cohavora_core
    )

    # Manual L3 harness. It is intentionally not registered with CTest because
    # it requires two real-service participants and operator-controlled faults.
    add_executable(test_stream_delivery_runtime
        ${LIVEKIT_TEST_SOURCE_DIR}/runtime/test_stream_delivery_runtime.cpp
    )
    target_link_libraries(test_stream_delivery_runtime PRIVATE
        cohavora_core
    )

    # S8b controlled two-peer media marker harness. It is built on demand but
    # never registered with CTest because it requires real service tokens.
    add_executable(test_e2e_media_runtime
        ${LIVEKIT_TEST_SOURCE_DIR}/runtime/test_e2e_media_runtime.cpp
    )
    target_include_directories(test_e2e_media_runtime PRIVATE
        ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_e2e_media_runtime PRIVATE cohavora_core)
endif()

if(LIVEKIT_BUILD_HARDWARE_TESTS)
    add_executable(test_wasapi_capture
        ${LIVEKIT_TEST_SOURCE_DIR}/test_wasapi_capture.cpp
    )
    target_include_directories(test_wasapi_capture PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_wasapi_capture PRIVATE
        cohavora_core
    )
endif()

if(WIN32)
    # The default invocation uses fake COM interfaces and opens no devices.
    # Real capture remains an explicit --camera-runtime invocation.
    add_executable(test_dshow_capture
        ${LIVEKIT_TEST_SOURCE_DIR}/test_dshow_capture.cpp
    )
    target_include_directories(test_dshow_capture PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_dshow_capture PRIVATE
        cohavora_core
    )
endif()

# First-audible-frame playout warmup unit test
add_executable(test_audio_playout_warmup
    ${LIVEKIT_TEST_SOURCE_DIR}/test_audio_playout_warmup.cpp
)
target_include_directories(test_audio_playout_warmup PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_audio_playout_warmup PRIVATE
    cohavora_core
)

# Screen-share lifecycle regression and opt-in real Windows capture probe.
add_executable(test_screen_share_session ${LIVEKIT_TEST_SOURCE_DIR}/test_screen_share_session.cpp)
target_include_directories(test_screen_share_session PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_screen_share_session PRIVATE cohavora_core)
add_test(NAME screen_share_session_test COMMAND test_screen_share_session)
set_tests_properties(screen_share_session_test PROPERTIES TIMEOUT 30 LABELS "CORE_REGRESSION")
add_executable(test_desktop_capture_runtime ${LIVEKIT_TEST_SOURCE_DIR}/runtime/test_desktop_capture_runtime.cpp)
target_include_directories(test_desktop_capture_runtime PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_desktop_capture_runtime PRIVATE cohavora_core)
add_executable(test_screen_share_runtime ${LIVEKIT_TEST_SOURCE_DIR}/runtime/test_screen_share_runtime.cpp)
target_include_directories(test_screen_share_runtime PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_screen_share_runtime PRIVATE cohavora_core)

# Opt-in M0 probe for native annotation overlays. It opens real desktop
# windows, moves the pointer inside an owned fixture, and captures one screen,
# so it is built for explicit runtime use but never registered in CTest.
add_executable(test_annotation_overlay_runtime
    ${LIVEKIT_TEST_SOURCE_DIR}/runtime/test_annotation_overlay_runtime.cpp)
livekit_configure_qt_test(test_annotation_overlay_runtime)
target_compile_options(test_annotation_overlay_runtime PRIVATE /utf-8)
# Opt-in desktop interaction; deliberately not part of device-free CTest.

# VP8 Simulcast Test
add_executable(test_simulcast
    ${LIVEKIT_TEST_SOURCE_DIR}/test_simulcast.cpp
)
target_include_directories(test_simulcast PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_simulcast PRIVATE
    cohavora_core
)

if(LIVEKIT_BUILD_EXTENDED_TESTS)
    add_executable(test_adaptive_stream
        ${LIVEKIT_TEST_SOURCE_DIR}/test_adaptive_stream.cpp
    )
    target_include_directories(test_adaptive_stream PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_adaptive_stream PRIVATE
        cohavora_core
    )

    add_executable(test_speaker_vad
        ${LIVEKIT_TEST_SOURCE_DIR}/test_speaker_vad.cpp
    )
    target_include_directories(test_speaker_vad PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_speaker_vad PRIVATE
        cohavora_core
    )

    add_executable(test_e2ee
        ${LIVEKIT_TEST_SOURCE_DIR}/test_e2ee.cpp
    )
    target_include_directories(test_e2ee PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
    target_link_libraries(test_e2ee PRIVATE
        cohavora_core
        ${OPENSSL_LIBRARIES}
    )
endif()

# Lifecycle & Reconnect Stress Test (GAP-02)
add_executable(test_stress_lifecycle
    ${LIVEKIT_TEST_SOURCE_DIR}/test_stress_lifecycle.cpp
)
target_link_libraries(test_stress_lifecycle PRIVATE
    cohavora_core
)

# Simulate Scenario Suite (Matching Flutter sendSimulateScenario)
add_executable(test_simulate_scenario
    ${LIVEKIT_TEST_SOURCE_DIR}/test_simulate_scenario.cpp
)
target_link_libraries(test_simulate_scenario PRIVATE
    cohavora_core
)

# Multi-Codec Simulcast & Backup Codecs Test (GAP-03)
add_executable(test_backup_codecs
    ${LIVEKIT_TEST_SOURCE_DIR}/test_backup_codecs.cpp
)
target_include_directories(test_backup_codecs PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_backup_codecs PRIVATE
    cohavora_core
)

# Multi-User Grid Math & Avatar Hash Test
add_executable(test_meeting_ui_grid
    ${LIVEKIT_TEST_SOURCE_DIR}/test_meeting_ui_grid.cpp
)
target_include_directories(test_meeting_ui_grid PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})

# Server-driven Single-PC negotiation, including zero additional media sections.
add_executable(test_single_pc_negotiation
    ${LIVEKIT_TEST_SOURCE_DIR}/test_single_pc_negotiation.cpp)
target_include_directories(test_single_pc_negotiation PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR})
target_link_libraries(test_single_pc_negotiation PRIVATE cohavora_core)
add_test(NAME single_pc_negotiation_test COMMAND test_single_pc_negotiation)
set_tests_properties(single_pc_negotiation_test PROPERTIES TIMEOUT 30 LABELS "CORE_REGRESSION")
