# GPU Video Converter Test
add_executable(test_gpu_converter
    ${LIVEKIT_TEST_SOURCE_DIR}/test_gpu_converter.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_types.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_shaders.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/gpu_video_converter.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/gpu_video_converter.cpp
)

target_include_directories(test_gpu_converter BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
)

target_link_libraries(test_gpu_converter PRIVATE
    cohavora_core
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Gui.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtfreetype.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtharfbuzz.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtlibpng.lib
    d3d11
    dxgi
    d3dcompiler
    imm32
    winmm
    wtsapi32
    version
    netapi32
    userenv
    ws2_32
    crypt32
)

target_compile_definitions(test_gpu_converter PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    NDEBUG
    _ITERATOR_DEBUG_LEVEL=0
    $<$<CONFIG:Debug>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
    $<$<CONFIG:Debug>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>
    LIVEKIT_DX11_TESTING
)

target_link_options(test_gpu_converter PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
)

set_target_properties(test_gpu_converter PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME gpu_converter_test COMMAND test_gpu_converter)

# DX11 renderer lifecycle test. It uses deterministic failure injection, so it
# does not require a GPU, a desktop session, or a visible native window.
add_executable(test_dx11_renderer_lifecycle
    ${LIVEKIT_TEST_SOURCE_DIR}/test_dx11_renderer_lifecycle.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_types.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_color_conversion.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_shaders.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_renderer.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_renderer.cpp
)

target_include_directories(test_dx11_renderer_lifecycle BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src
)

target_link_libraries(test_dx11_renderer_lifecycle PRIVATE
    d3d11
    dxgi
    d3dcompiler
)

target_compile_definitions(test_dx11_renderer_lifecycle PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    LIVEKIT_DX11_TESTING
)

if(MSVC)
    # dx11_shaders.h contains UTF-8 comments and embedded HLSL source. Match
    # the application/test compiler encoding so raw string literals are parsed
    # consistently by MSVC.
    target_compile_options(test_dx11_renderer_lifecycle PRIVATE /utf-8)
endif()

set_target_properties(test_dx11_renderer_lifecycle PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME dx11_renderer_lifecycle_test COMMAND test_dx11_renderer_lifecycle)

# Shader-side colour-policy test. It is deterministic and does not need a
# desktop session or a D3D device.
add_executable(test_dx11_color_conversion
    ${LIVEKIT_TEST_SOURCE_DIR}/test_dx11_color_conversion.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_color_conversion.h
)

target_include_directories(test_dx11_color_conversion BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
)

target_link_libraries(test_dx11_color_conversion PRIVATE
    cohavora_core
)

set_target_properties(test_dx11_color_conversion PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME dx11_color_conversion_test COMMAND test_dx11_color_conversion)

# Compile the embedded HLSL without creating a device or native window. This
# catches shader/register-layout regressions in CI and remote sessions.
add_executable(test_dx11_shaders
    ${LIVEKIT_TEST_SOURCE_DIR}/test_dx11_shaders.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_shaders.h
)

target_include_directories(test_dx11_shaders BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
)

target_link_libraries(test_dx11_shaders PRIVATE
    d3dcompiler
)

if(MSVC)
    target_compile_options(test_dx11_shaders PRIVATE /utf-8)
endif()

set_target_properties(test_dx11_shaders PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME dx11_shaders_test COMMAND test_dx11_shaders)

# Owned I420 frame and cancellable render subscription contract test.
add_executable(test_owned_i420_frame
    ${LIVEKIT_TEST_SOURCE_DIR}/test_owned_i420_frame.cpp
)

target_link_libraries(test_owned_i420_frame PRIVATE
    cohavora_core
)

set_target_properties(test_owned_i420_frame PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME owned_i420_frame_test COMMAND test_owned_i420_frame)

add_executable(test_video_render_router
    ${LIVEKIT_TEST_SOURCE_DIR}/test_video_render_router.cpp
)

target_link_libraries(test_video_render_router PRIVATE
    cohavora_core
)

set_target_properties(test_video_render_router PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME video_render_router_test COMMAND test_video_render_router)

add_executable(test_qt_cpu_video_renderer
    ${LIVEKIT_TEST_SOURCE_DIR}/test_qt_cpu_video_renderer.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/render/qt_cpu_video_renderer.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/render/qt_cpu_video_renderer.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/render/video_render_session.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/render/video_render_session.cpp
)

target_include_directories(test_qt_cpu_video_renderer BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${WEBRTC_ROOT}/include/third_party/libyuv/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
)

target_link_libraries(test_qt_cpu_video_renderer PRIVATE
    cohavora_core
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Gui.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtfreetype.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtharfbuzz.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtlibpng.lib
    imm32
    winmm
    wtsapi32
    version
    netapi32
    userenv
    ws2_32
    crypt32
)

target_compile_definitions(test_qt_cpu_video_renderer PRIVATE
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

target_link_options(test_qt_cpu_video_renderer PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
)

set_target_properties(test_qt_cpu_video_renderer PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME qt_cpu_video_renderer_test COMMAND test_qt_cpu_video_renderer)

# Concurrent nine-stream latest-wins/teardown test.  It shares the CPU
# renderer linkage because VideoRenderSession owns the fallback backend even
# though this test selects DX11's I420 callback path only.
add_executable(test_video_render_session_stress
    ${LIVEKIT_TEST_SOURCE_DIR}/test_video_render_session_stress.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/render/qt_cpu_video_renderer.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/render/qt_cpu_video_renderer.cpp
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/render/video_render_session.h
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/render/video_render_session.cpp
)

target_include_directories(test_video_render_session_stress BEFORE PRIVATE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${WEBRTC_ROOT}/include/third_party/libyuv/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
)

target_link_libraries(test_video_render_session_stress PRIVATE
    cohavora_core
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Gui.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtfreetype.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtharfbuzz.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtlibpng.lib
    imm32
    winmm
    wtsapi32
    version
    netapi32
    userenv
    ws2_32
    crypt32
)

target_compile_definitions(test_video_render_session_stress PRIVATE
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

target_link_options(test_video_render_session_stress PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
)

set_target_properties(test_video_render_session_stress PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME video_render_session_stress_test COMMAND test_video_render_session_stress)

set_tests_properties(
    always_active_checks_test
    camera_owner_remediation_test
    log_redaction_test
    connection_log_redaction_test
    qt_log_redaction_test
    signaling_tests
    signaling_url_policy_test
    room_state_events_test
    remote_publication_lifecycle_test
    local_unpublish_transaction_test
    unpublish_lifetime_remediation_test
    meeting_recovery_ux_test
    stream_writer_lifetime_remediation_test
    stress_lifecycle_test
    http_owner_remediation_test
    meeting_session_runtime_test
    participant_snapshot_remediation_test
    meeting_startup_transaction_test
    PROPERTIES LABELS "CORE_REGRESSION"
)
set_property(TEST meeting_session_runtime_test
    APPEND PROPERTY LABELS "TELEMETRY_S1")
set_property(TEST signaling_tests signaling_url_policy_test
    APPEND PROPERTY LABELS "PR_SEC_006_FOCUSED")
set_property(TEST always_active_checks_test
    APPEND PROPERTY LABELS "ASSERTION_REGRESSION")
