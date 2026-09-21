# OpenMeeting HTTP Client Test
add_executable(test_openmeeting_http
    ${LIVEKIT_TEST_SOURCE_DIR}/test_openmeeting_http.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/http_types.h
)
target_include_directories(test_openmeeting_http BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${GEN_DIR}
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtNetwork
)
target_link_libraries(test_openmeeting_http PRIVATE
    cohavora_core
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Network.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    netapi32
    userenv
    version
    ws2_32
    crypt32
    dnsapi
    iphlpapi
    secur32
)

target_compile_definitions(test_openmeeting_http PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    NDEBUG
    _ITERATOR_DEBUG_LEVEL=0
    $<$<CONFIG:Debug>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
    $<$<CONFIG:Debug>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>
)

target_link_options(test_openmeeting_http PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
)

set_target_properties(test_openmeeting_http PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
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
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/meeting_coordinator.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/meeting_coordinator.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/http_types.h
)
target_include_directories(test_http_admission_owner BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${GEN_DIR}
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtWidgets
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtNetwork
)
target_link_libraries(test_http_admission_owner PRIVATE
    cohavora_core
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Gui.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Widgets.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Network.lib
    ${TDESKTOP_LIBS_DIR}/zlib/Release/zlibstatic.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtfreetype.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtharfbuzz.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtlibpng.lib
    netapi32
    userenv
    version
    ws2_32
    crypt32
    dnsapi
    iphlpapi
    secur32
    imm32
    winmm
    wtsapi32
)
target_compile_definitions(test_http_admission_owner PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    ASIO_STANDALONE
    WEBRTC_WIN
    NDEBUG
    _ITERATOR_DEBUG_LEVEL=0
    $<$<CONFIG:Debug>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
    $<$<CONFIG:Debug>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>
)
target_link_options(test_http_admission_owner PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
)
set_target_properties(test_http_admission_owner PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME http_owner_remediation_test COMMAND test_http_admission_owner --debug)
set_tests_properties(http_owner_remediation_test PROPERTIES TIMEOUT 60)

# Coordinator session runtime serialization test
add_executable(test_meeting_session_runtime
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/restored/test_meeting_session_runtime.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/meeting_session_runtime.h
)
target_include_directories(test_meeting_session_runtime BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/rtc
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/rpc
    ${WEBRTC_ROOT}/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
)
target_link_libraries(test_meeting_session_runtime PRIVATE
    asio::asio
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    netapi32
    userenv
    version
    winmm
)
target_compile_definitions(test_meeting_session_runtime PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    ASIO_STANDALONE
    NDEBUG
    _ITERATOR_DEBUG_LEVEL=0
    $<$<CONFIG:Debug>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
    $<$<CONFIG:Debug>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>
)
if(MSVC)
    target_compile_options(test_meeting_session_runtime PRIVATE /utf-8)
endif()
target_link_options(test_meeting_session_runtime PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
)
set_target_properties(test_meeting_session_runtime PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME meeting_session_runtime_test COMMAND test_meeting_session_runtime)

# IDA2-P0-001: immutable participant snapshots, incarnation tickets, ordered
# Room delivery, and delayed Qt projection rejection for retired instances.
add_executable(test_participant_snapshot_remediation
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_participant_snapshot_remediation.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/meeting_coordinator.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/meeting_coordinator.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/http_types.h
)
target_include_directories(test_participant_snapshot_remediation BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${GEN_DIR}
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtWidgets
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtNetwork
)
target_link_libraries(test_participant_snapshot_remediation PRIVATE
    cohavora_core
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Gui.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Widgets.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Network.lib
    ${TDESKTOP_LIBS_DIR}/zlib/Release/zlibstatic.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtfreetype.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtharfbuzz.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtlibpng.lib
    netapi32
    userenv
    version
    ws2_32
    crypt32
    dnsapi
    iphlpapi
    secur32
    imm32
    winmm
    wtsapi32
)
target_compile_definitions(test_participant_snapshot_remediation PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    ASIO_STANDALONE
    WEBRTC_WIN
    NDEBUG
    _ITERATOR_DEBUG_LEVEL=0
    $<$<CONFIG:Debug>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
    $<$<CONFIG:Debug>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>
)
target_link_options(test_participant_snapshot_remediation PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
)
set_target_properties(test_participant_snapshot_remediation PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME participant_snapshot_remediation_test
    COMMAND test_participant_snapshot_remediation)
set_tests_properties(participant_snapshot_remediation_test PROPERTIES TIMEOUT 60)

# IDA2 window acceptance uses the same allowed test source with a distinct
# QApplication branch. It links the production console exactly once; the core
# target keeps its original synchronous LogToConsole hook and owner cases.
add_executable(test_participant_window_remediation
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_participant_snapshot_remediation.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
    ${LIVEKIT_WINDOW_FIXTURE_SOURCES}
)
target_include_directories(test_participant_window_remediation BEFORE PRIVATE
    ${WEBRTC_ROOT}/include/third_party/perfetto/include
    ${WEBRTC_ROOT}/include/out-x64-release/gen/third_party/perfetto
    ${WEBRTC_ROOT}/include/out-x64-release/gen/third_party/perfetto/build_config
)
livekit_configure_qt_test(test_participant_window_remediation)
target_compile_definitions(test_participant_window_remediation PRIVATE IDA2_WINDOW_ACCEPTANCE)
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
    NOMINMAX
    NDEBUG
)
set_target_properties(test_meeting_startup_transaction PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME meeting_startup_transaction_test COMMAND test_meeting_startup_transaction)
