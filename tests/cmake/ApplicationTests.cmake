# PR-SEC-002: isolated credential storage, auth response ordering, and real login UI.
add_executable(test_debug_login_options
    ${LIVEKIT_TEST_SOURCE_DIR}/test_debug_login_options.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/app/debug_login_options.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/app/debug_login_options.h
)
livekit_configure_qt_test(test_debug_login_options)
add_test(NAME debug_login_options_test COMMAND test_debug_login_options)
set_tests_properties(debug_login_options_test PROPERTIES
    TIMEOUT 30 LABELS "PR_SEC_004_FOCUSED;CORE_REGRESSION")

add_executable(test_session_credentials
    ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_session_credentials.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/login_dialog.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/login_dialog.h
)
livekit_configure_qt_test(test_session_credentials)
target_link_libraries(test_session_credentials PRIVATE crypt32)
add_test(NAME session_credentials_test COMMAND test_session_credentials --debug)
set_tests_properties(session_credentials_test PROPERTIES
    TIMEOUT 60 LABELS "CORE_REGRESSION")
set_property(TEST session_credentials_test openmeeting_http_test openmeeting_http_watchdog_test
    http_owner_remediation_test APPEND PROPERTY LABELS "PR_SEC_002_FOCUSED")

# Typed meeting catalog contracts and Qt owner ordering/lifetime behavior.
add_executable(test_meeting_catalog
    ${LIVEKIT_TEST_SOURCE_DIR}/test_meeting_catalog.cpp
    ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/meeting_list_model.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/meeting_list_model.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/meeting_catalog_controller.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/core/meeting_catalog_controller.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/http_types.h
)
target_include_directories(test_meeting_catalog BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtNetwork
)
target_link_libraries(test_meeting_catalog PRIVATE
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Network.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    OpenSSL::SSL OpenSSL::Crypto
    netapi32 userenv version ws2_32 crypt32 dnsapi iphlpapi secur32 winmm
)
target_compile_definitions(test_meeting_catalog PRIVATE
    WIN32 _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS NDEBUG _ITERATOR_DEBUG_LEVEL=0
    $<$<CONFIG:Debug>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
    $<$<CONFIG:Debug>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>
)
target_compile_options(test_meeting_catalog PRIVATE /utf-8)
target_link_options(test_meeting_catalog PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
)
set_target_properties(test_meeting_catalog PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
add_test(NAME meeting_catalog_test COMMAND test_meeting_catalog)
set_tests_properties(meeting_catalog_test PROPERTIES TIMEOUT 60 LABELS "CORE_REGRESSION")

# PR-SEC-004: strict startup and explicit runtime --debug HTTP policy.
foreach(_policy_variant strict dev)
    set(_policy_target test_http_transport_policy_${_policy_variant})
    add_executable(${_policy_target}
        ${LIVEKIT_TEST_SOURCE_DIR}/remediation/test_http_transport_policy.cpp
        ${LIVEKIT_TEST_SOURCE_DIR}/support/test_check.h
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.cpp
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/credential_store.h
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.cpp
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/service_endpoint_policy.h
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.cpp
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/session_manager.h
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.cpp
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/meeting_types.h
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.cpp
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/net/openmeeting_http_client.h
    )
    target_include_directories(${_policy_target} BEFORE PRIVATE
        ${LIVEKIT_PROJECT_SOURCE_DIR}
        ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
        ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
        ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtNetwork
    )
    target_link_libraries(${_policy_target} PRIVATE
        ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
        ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Network.lib
        ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
        OpenSSL::SSL OpenSSL::Crypto
        netapi32 userenv version ws2_32 crypt32 dnsapi iphlpapi secur32 winmm
    )
    target_compile_definitions(${_policy_target} PRIVATE
        WIN32 _WINDOWS WIN32_LEAN_AND_MEAN NOMINMAX
        _WINSOCK_DEPRECATED_NO_WARNINGS NDEBUG _ITERATOR_DEBUG_LEVEL=0
        $<$<CONFIG:Debug>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
        $<$<CONFIG:Debug>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>
    )
    target_compile_options(${_policy_target} PRIVATE /utf-8)
    if(_policy_variant STREQUAL "dev")
        target_compile_definitions(${_policy_target} PRIVATE LIVEKIT_EXPECT_DEBUG_HTTP)
    endif()
    target_link_options(${_policy_target} PRIVATE
        $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
        $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
    )
    set_target_properties(${_policy_target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
    if(_policy_variant STREQUAL "dev")
        add_test(NAME http_transport_policy_${_policy_variant}_test COMMAND ${_policy_target} --debug)
    else()
        add_test(NAME http_transport_policy_${_policy_variant}_test COMMAND ${_policy_target} --debugger)
    endif()
    set_tests_properties(http_transport_policy_${_policy_variant}_test PROPERTIES
        TIMEOUT 30 LABELS "PR_SEC_004_FOCUSED")
endforeach()

# Deploy whichever modules exist on this platform. The loader itself stays
# portable; on Unix its consumers link CMAKE_DL_LIBS above (also inherited by
# test_participant_window_remediation from test_camera_owner_remediation).
livekit_deploy_renderer(test_camera_owner_remediation)
livekit_deploy_renderer(test_participant_window_remediation)

# These contracts and the production-window fixture use HWND/Win32 APIs.
# Keep their fixtures, D3D links and target-file expressions out of Unix builds.
if(WIN32)
# P2 focused module contract + actual DX11 lifetime gate.
add_library(cohavora_render_dx11_test MODULE
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/render/modules/dx11_backend.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_renderer.cpp)
set_target_properties(cohavora_render_dx11_test PROPERTIES AUTOMOC OFF
    PREFIX "" OUTPUT_NAME "cohavora-render-dx11-test" CXX_VISIBILITY_PRESET hidden)
target_include_directories(cohavora_render_dx11_test PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR} ${LIVEKIT_PROJECT_SOURCE_DIR}/src
    ${LIVEKIT_TEST_SOURCE_DIR}/render_p2)
target_compile_definitions(cohavora_render_dx11_test PRIVATE
    LK_RENDER_MODULE_BUILD LIVEKIT_DX11_MODULE_TESTING=1)
target_link_libraries(cohavora_render_dx11_test PRIVATE d3d11 dxgi d3dcompiler)
if(MSVC)
    target_compile_options(cohavora_render_dx11_test PRIVATE /utf-8)
endif()
add_dependencies(test_participant_window_remediation cohavora_render_dx11_test)

add_executable(test_render_module ${LIVEKIT_TEST_SOURCE_DIR}/render_p2/test_render_module.cpp ${LIVEKIT_PROJECT_SOURCE_DIR}/src/render/backend_module.cpp)
set_target_properties(test_render_module PROPERTIES AUTOMOC OFF)
target_include_directories(test_render_module PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR} ${LIVEKIT_PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_render_module PRIVATE d3d11 dxgi)
foreach(variant missing_entry wrong_version missing_function wrong_capabilities create_failure)
    add_library(render_fixture_${variant} MODULE ${LIVEKIT_TEST_SOURCE_DIR}/render_p2/invalid_module.c)
    set_target_properties(render_fixture_${variant} PROPERTIES AUTOMOC OFF)
    target_include_directories(render_fixture_${variant} PRIVATE ${LIVEKIT_PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(render_fixture_${variant} PRIVATE LK_RENDER_MODULE_BUILD FIXTURE_${variant})
    add_dependencies(test_render_module render_fixture_${variant})
endforeach()
livekit_deploy_renderer(test_render_module)
add_dependencies(test_participant_window_remediation render_fixture_wrong_version render_fixture_create_failure)
add_test(NAME render_module_fallback_test COMMAND test_participant_window_remediation --module-fallback
    $<TARGET_FILE:render_fixture_wrong_version> $<TARGET_FILE:render_fixture_create_failure>)
set_tests_properties(render_module_fallback_test PROPERTIES
    TIMEOUT 45 LABELS "CORE_REGRESSION;RENDER_MODULE_FOCUSED")

# The GL ABI contract uses a fake dispatch table and needs no context, GPU or desktop.
target_sources(test_participant_window_remediation PRIVATE ${LIVEKIT_TEST_SOURCE_DIR}/render_p3/opengl_contract.cpp)
add_test(NAME render_opengl_contract_test COMMAND test_participant_window_remediation --opengl-contract)
set_tests_properties(render_opengl_contract_test PROPERTIES
    TIMEOUT 45 LABELS "RENDER_CONTRACT")

# These cases need a real native window, GPU/context, desktop capture or driver
# observer. Labels classify them; this option is what keeps them out of default CTest.
if(LIVEKIT_BUILD_RENDERER_RUNTIME_TESTS)
    add_test(NAME render_dx11_owner_test COMMAND test_participant_window_remediation
        --dx11-owner $<TARGET_FILE:cohavora_render_dx11_test>)
    add_test(NAME render_module_test COMMAND test_render_module
        $<TARGET_FILE:cohavora_render_dx11>
        $<TARGET_FILE:render_fixture_missing_entry>
        $<TARGET_FILE:render_fixture_wrong_version>
        $<TARGET_FILE:render_fixture_missing_function>
        $<TARGET_FILE:render_fixture_wrong_capabilities>)
    add_test(NAME render_module_window_test COMMAND test_participant_window_remediation --module-lifecycle)
    add_test(NAME render_opengl_rapid_rebuild_test COMMAND test_participant_window_remediation
        --opengl-rapid-rebuild)
    add_test(NAME render_opengl_window_test COMMAND test_participant_window_remediation --opengl-window)
    add_test(NAME render_opengl_window_hidpi_test COMMAND test_participant_window_remediation --opengl-window)
    add_test(NAME render_backend_diagnostics_test COMMAND test_participant_window_remediation --opengl-diagnostics)
    add_test(NAME render_multisession_lifecycle_test COMMAND test_participant_window_remediation
        --render-multisession-lifecycle)
    add_test(NAME render_angle_tdr_observer_test COMMAND test_participant_window_remediation
        --opengl-driver-loss-smoke --output "${CMAKE_BINARY_DIR}/render-angle-tdr/smoke")
    add_test(NAME render_dx11_tdr_observer_test COMMAND test_participant_window_remediation
        --dx11-driver-loss-smoke --output "${CMAKE_BINARY_DIR}/render-dx11-tdr/smoke")

    set_tests_properties(render_dx11_owner_test render_module_test render_module_window_test
        render_opengl_rapid_rebuild_test render_backend_diagnostics_test
        render_multisession_lifecycle_test render_angle_tdr_observer_test
        render_dx11_tdr_observer_test PROPERTIES
        TIMEOUT 60 RUN_SERIAL TRUE LABELS "RENDER_RUNTIME" ENVIRONMENT "QT_SCALE_FACTOR=1")
    set_tests_properties(render_opengl_window_test render_opengl_window_hidpi_test PROPERTIES
        TIMEOUT 60 RUN_SERIAL TRUE LABELS "RENDER_RUNTIME;RENDER_PIXEL_RUNTIME")
    set_tests_properties(render_opengl_window_test PROPERTIES ENVIRONMENT
        "QT_SCALE_FACTOR=1;LIVEKIT_PRESENTATION_EVIDENCE_DIR=${CMAKE_BINARY_DIR}/render-recovery/dpi100")
    set_tests_properties(render_opengl_window_hidpi_test PROPERTIES ENVIRONMENT
        "QT_SCALE_FACTOR=1.5;LIVEKIT_PRESENTATION_EVIDENCE_DIR=${CMAKE_BINARY_DIR}/render-recovery/dpi150")

    # This is a real-service observer with injected loss, so it requires both
    # explicit runtime and external-service opt-ins. Real TDR trigger modes stay manual.
    if(LIVEKIT_BUILD_EXTERNAL_TESTS)
        add_test(NAME render_live_meeting_recovery_test COMMAND test_participant_window_remediation
            --live-meeting-dx11-driver-loss-smoke
            --output "${CMAKE_BINARY_DIR}/render-live-meeting-tdr/smoke")
        set_tests_properties(render_live_meeting_recovery_test PROPERTIES
            TIMEOUT 90 RUN_SERIAL TRUE LABELS "RENDER_RUNTIME;EXTERNAL_RUNTIME"
            ENVIRONMENT "QT_SCALE_FACTOR=1")
    endif()
endif()
endif()

# All Qt consumers use the same compiled startup theme target.
foreach(ui_theme_consumer IN ITEMS test_camera_owner_remediation
        test_participant_window_remediation test_session_credentials test_qt_log_redaction)
    target_link_libraries(${ui_theme_consumer} PRIVATE cohavora_ui_theme)
endforeach()

if(LIVEKIT_LRELEASE_EXECUTABLE)
    add_executable(test_ui_presentation
        ${LIVEKIT_TEST_SOURCE_DIR}/test_ui_presentation.cpp)
    livekit_configure_qt_test(test_ui_presentation)
    target_link_libraries(test_ui_presentation PRIVATE
        cohavora_ui_theme
        cohavora_ui_translations)
    target_compile_options(test_ui_presentation PRIVATE /utf-8)
    add_test(NAME ui_presentation_test COMMAND test_ui_presentation)
    set_tests_properties(ui_presentation_test PROPERTIES TIMEOUT 30 LABELS "UI_PRESENTATION")
endif()
