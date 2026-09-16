cmake_minimum_required(VERSION 3.20)
project(livekit_signaling_cpp LANGUAGES C CXX)

option(LIVEKIT_BUILD_EXTENDED_TESTS
    "Build optional feature tests (APM, permissions, adaptive stream, VAD, E2EE)"
    OFF)
option(LIVEKIT_BUILD_HARDWARE_TESTS
    "Build tests that enumerate or open local WASAPI/DirectShow devices"
    OFF)
option(LIVEKIT_BUILD_EXTERNAL_TESTS
    "Build tests that connect to an external LiveKit server"
    OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)

set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG "Release;None")

if(MSVC)
    add_link_options(/FORCE:MULTIPLE)
endif()

# Find Packages via CMake / Vcpkg
find_package(asio CONFIG REQUIRED)
find_package(Protobuf REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_compile_definitions(
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    CRL_USE_WINAPI
    ASIO_STANDALONE
)

if(MSVC)
    add_compile_options(/FS /Z7)
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()

# Protobuf Generation Setup
set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protos)
set(PROTO_FILES
    ${PROTO_DIR}/logger/options.proto
    ${PROTO_DIR}/livekit_metrics.proto
    ${PROTO_DIR}/livekit_models.proto
    ${PROTO_DIR}/livekit_rtc.proto
    ${PROTO_DIR}/openmeeting_meeting.proto
)

set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

# Get protoc executable
if(TARGET protobuf::protoc)
  set(PROTOC_EXE "$<TARGET_FILE:protobuf::protoc>")
else()
  find_program(PROTOC_EXE NAMES protoc REQUIRED)
endif()

# Custom command to generate protobuf sources
set(PROTO_SRCS
    ${GEN_DIR}/logger/options.pb.cc
    ${GEN_DIR}/livekit_metrics.pb.cc
    ${GEN_DIR}/livekit_models.pb.cc
    ${GEN_DIR}/livekit_rtc.pb.cc
    ${GEN_DIR}/openmeeting_meeting.pb.cc
)
set(PROTO_HDRS
    ${GEN_DIR}/logger/options.pb.h
    ${GEN_DIR}/livekit_metrics.pb.h
    ${GEN_DIR}/livekit_models.pb.h
    ${GEN_DIR}/livekit_rtc.pb.h
    ${GEN_DIR}/openmeeting_meeting.pb.h
)

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
    COMMAND ${PROTOC_EXE}
        --proto_path=${PROTO_DIR}
        --cpp_out=${GEN_DIR}
        ${PROTO_FILES}
    DEPENDS ${PROTO_FILES}
    COMMENT "Generating C++ Protobuf files from options, metrics, models, and rtc schemas"
    VERBATIM
)

# 源文件列表
set(LIVEKIT_SOURCES
    src/signal/websocket_client.cpp
    src/signal/region_provider.cpp
    src/signal/signal_stream.cpp
    src/signal/signal_client.cpp
    src/core/participant.cpp
    src/core/room.cpp
    src/core/local_audio_track.cpp
    src/core/local_video_track.cpp
    src/core/remote_track_publication.cpp
    src/core/adaptive_stream_manager.cpp
    src/core/data_stream_assembler.cpp
    src/rtc/webrtc_manager.cpp
    src/rtc/audio_frame.cpp
    src/rtc/video_frame.cpp
    src/rtc/audio_source.cpp
    src/rtc/video_source.cpp
    src/rtc/audio_stream.cpp
    src/rtc/video_stream.cpp
    src/rtc/rtc_audio_source.cpp
    src/rtc/rtc_video_source.cpp
    src/render/owned_i420_frame.cpp
    src/render/video_render_router.cpp
    src/media/media_converters.cpp
    src/media/wasapi_enumerator.cpp
    src/media/wasapi_capture.cpp
    src/media/dshow_enumerator.cpp
    src/media/dshow_capture.cpp
    src/media/camera_source_manager.cpp
    src/media/audio_vad.cpp
    src/media/audio_apm.cpp
    src/e2ee/key_provider.cpp
    src/e2ee/frame_cryptor.cpp
    src/telemetry/crash_handler.cpp
    src/telemetry/stats_collector.cpp
)

# 头文件列表
set(LIVEKIT_HEADERS
    src/media/audio_apm.h
    src/signal/websocket_client.h
    src/signal/region_provider.h
    src/signal/signal_stream.h
    src/signal/signal_client.h
    src/core/room.h
    src/core/operation.h
    src/core/participant.h
    src/core/track.h
    src/core/local_audio_track.h
    src/core/local_video_track.h
    src/core/remote_track_publication.h
    src/core/adaptive_stream_manager.h
    src/core/data_stream_assembler.h
    src/rtc/webrtc_manager.h
    src/rtc/audio_frame.h
    src/rtc/video_frame.h
    src/rtc/audio_source.h
    src/rtc/video_source.h
    src/rtc/audio_stream.h
    src/rtc/video_stream.h
    src/rtc/rtc_audio_source.h
    src/rtc/rtc_video_source.h
    src/render/owned_i420_frame.h
    src/render/video_render_router.h
    src/media/media_converters.h
    src/media/wasapi_types.h
    src/media/wasapi_enumerator.h
    src/media/wasapi_capture.h
    src/media/dshow_types.h
    src/media/dshow_enumerator.h
    src/media/dshow_capture.h
    src/media/audio_vad.h
    src/e2ee/key_provider.h
    src/e2ee/frame_cryptor.h
    src/rpc/rpc_types.h
    src/rpc/chat_message.h
    src/telemetry/stats.h
    src/telemetry/stats_collector.h
    src/telemetry/telemetry.h
    src/telemetry/crash_handler.h
    src/telemetry/safe_spawn.h
    src/telemetry/coro_allocator.h
)

# 源文件分组
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${LIVEKIT_SOURCES})

# 头文件分组
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Header Files" FILES ${LIVEKIT_HEADERS})

# Main Signaling Library
add_library(livekit_signaling STATIC
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    ${LIVEKIT_SOURCES}
    ${LIVEKIT_HEADERS}
)

# Project Local Dependencies Directory (./deps)
if(NOT DEFINED DEPS_DIR)
    set(DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps")
endif()

# WebRTC Root Directory
if(NOT DEFINED WEBRTC_ROOT)
    if(DEFINED ENV{WEBRTC_ROOT})
        set(WEBRTC_ROOT "$ENV{WEBRTC_ROOT}")
    else()
        set(WEBRTC_ROOT "${DEPS_DIR}/webrtc")
    endif()
endif()

target_include_directories(livekit_signaling BEFORE PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/signal
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rtc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/media
    ${CMAKE_CURRENT_SOURCE_DIR}/src/e2ee
    ${CMAKE_CURRENT_SOURCE_DIR}/src/rpc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/telemetry
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app
    ${GEN_DIR}
    ${WEBRTC_ROOT}/include/third_party/boringssl/src/include
    ${WEBRTC_ROOT}/include
)

# WebRTC Library Multi-Config Resolution (Debug vs Release)
if(EXISTS "${WEBRTC_ROOT}/lib/Debug/webrtc.lib")
    set(WEBRTC_DEBUG_LIB "${WEBRTC_ROOT}/lib/Debug/webrtc.lib")
elseif(EXISTS "${WEBRTC_ROOT}/lib/webrtc_d.lib")
    set(WEBRTC_DEBUG_LIB "${WEBRTC_ROOT}/lib/webrtc_d.lib")
else()
    set(WEBRTC_DEBUG_LIB "${WEBRTC_ROOT}/lib/webrtc.lib")
endif()

if(EXISTS "${WEBRTC_ROOT}/lib/Release/webrtc.lib")
    set(WEBRTC_RELEASE_LIB "${WEBRTC_ROOT}/lib/Release/webrtc.lib")
else()
    set(WEBRTC_RELEASE_LIB "${WEBRTC_ROOT}/lib/webrtc.lib")
endif()

message(STATUS "[WebRTC] Using Debug Library   : ${WEBRTC_DEBUG_LIB}")
message(STATUS "[WebRTC] Using Release Library : ${WEBRTC_RELEASE_LIB}")

add_compile_definitions(
    WEBRTC_WIN
    NOMINMAX
    NDEBUG
    RTC_DISABLE_CHECK_MSG=1
    RTC_DISABLE_CHECKING=1
    RTC_NO_UNIQUE_ADDRESS=
    _ITERATOR_DEBUG_LEVEL=0
)

target_link_libraries(livekit_signaling PUBLIC
    $<$<CONFIG:Debug>:${WEBRTC_DEBUG_LIB}>
    $<$<NOT:$<CONFIG:Debug>>:${WEBRTC_RELEASE_LIB}>
    asio::asio
    protobuf::libprotobuf
    spdlog::spdlog
    ZLIB::ZLIB
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    ws2_32
    winmm
    secur32
    crypt32
    iphlpapi
    dmoguids
    msdmo
    amstrmid
    wmcodecdspuuid
    avrt
    strmiids
    ole32
    oleaut32
    propsys
)

# Unit/Integration Tests
add_executable(livekit_signaling_tests_run_v4
    tests/main.cpp
)

target_link_libraries(livekit_signaling_tests_run_v4 PRIVATE
    livekit_signaling
)

# Panic Guard Test
add_executable(test_panic_guard
    tests/test_panic_guard.cpp
)
target_link_libraries(test_panic_guard PRIVATE
    livekit_signaling
)

# Advanced Communication Test
add_executable(test_advanced_communication
    tests/test_advanced_communication.cpp
)
target_link_libraries(test_advanced_communication PRIVATE
    livekit_signaling
)

# Chunking & Simulcast Test
add_executable(test_chunking_simulcast
    tests/test_chunking_simulcast.cpp
)
target_link_libraries(test_chunking_simulcast PRIVATE
    livekit_signaling
)

# Inbound DataStream assembler unit test
add_executable(test_data_stream_assembler
    tests/test_data_stream_assembler.cpp
)
target_link_libraries(test_data_stream_assembler PRIVATE
    livekit_signaling
)

if(LIVEKIT_BUILD_EXTENDED_TESTS)
    add_executable(test_apm_3a
        tests/test_apm_3a.cpp
    )
    target_link_libraries(test_apm_3a PRIVATE
        livekit_signaling
    )

    add_executable(test_attributes_permissions_quality
        tests/test_attributes_permissions_quality.cpp
    )
    target_link_libraries(test_attributes_permissions_quality PRIVATE
        livekit_signaling
    )
endif()

# RPC System Test
add_executable(test_rpc_system
    tests/test_rpc_system.cpp
)
target_link_libraries(test_rpc_system PRIVATE
    livekit_signaling
)

# Media Streaming Test
add_executable(test_media_streaming
    tests/test_media_streaming.cpp
)
target_link_libraries(test_media_streaming PRIVATE
    livekit_signaling
)

# Stats System Test
add_executable(test_stats_system
    tests/test_stats_system.cpp
)
target_link_libraries(test_stats_system PRIVATE
    livekit_signaling
)

# Server room-state delta contract: room metadata, connection quality, stream
# state, and subscription permissions must commit before listener delivery.
add_executable(test_room_state_events
    tests/test_room_state_events.cpp
)
target_link_libraries(test_room_state_events PRIVATE
    livekit_signaling
)

# Canonical remote-publication ownership, control dispatch, and stale-session
# rejection. This exercises the P1 lifecycle independently of live media.
add_executable(test_remote_publication_lifecycle
    tests/test_remote_publication_lifecycle.cpp
)
target_link_libraries(test_remote_publication_lifecycle PRIVATE
    livekit_signaling
)

# Local media unpublish is committed by publisher SDP answer, not by a guessed
# SignalRequest. This locks down the protocol gate and public API validation.
add_executable(test_local_unpublish_transaction
    tests/test_local_unpublish_transaction.cpp
)
target_link_libraries(test_local_unpublish_transaction PRIVATE
    livekit_signaling
)

# Active camera source hot-switch transaction with first-frame verification and rollback
add_executable(test_camera_switch_transaction
    tests/test_camera_switch_transaction.cpp
)
target_link_libraries(test_camera_switch_transaction PRIVATE
    livekit_signaling
)

# AEC playout render reference integration with framing and reset lifecycle
add_executable(test_audio_render_reference_apm
    tests/test_audio_render_reference_apm.cpp
)
target_link_libraries(test_audio_render_reference_apm PRIVATE
    livekit_signaling
)

# Meeting recovery UX and connection state loopback contract
add_executable(test_meeting_recovery_ux
    tests/test_meeting_recovery_ux.cpp
)
target_link_libraries(test_meeting_recovery_ux PRIVATE
    livekit_signaling
)

if(LIVEKIT_BUILD_EXTERNAL_TESTS)
    add_executable(test_livekit_official_connect
        tests/test_livekit_official_connect.cpp
    )
    target_link_libraries(test_livekit_official_connect PRIVATE
        livekit_signaling
    )
endif()

if(LIVEKIT_BUILD_HARDWARE_TESTS)
    add_executable(test_wasapi_capture
        tests/test_wasapi_capture.cpp
    )
    target_link_libraries(test_wasapi_capture PRIVATE
        livekit_signaling
    )

    add_executable(test_dshow_capture
        tests/test_dshow_capture.cpp
    )
    target_link_libraries(test_dshow_capture PRIVATE
        livekit_signaling
    )
endif()

# First-audible-frame playout warmup unit test
add_executable(test_audio_playout_warmup
    tests/test_audio_playout_warmup.cpp
)
target_link_libraries(test_audio_playout_warmup PRIVATE
    livekit_signaling
)

# VP8 Simulcast Test
add_executable(test_simulcast
    tests/test_simulcast.cpp
)
target_link_libraries(test_simulcast PRIVATE
    livekit_signaling
)

if(LIVEKIT_BUILD_EXTENDED_TESTS)
    add_executable(test_adaptive_stream
        tests/test_adaptive_stream.cpp
    )
    target_link_libraries(test_adaptive_stream PRIVATE
        livekit_signaling
    )

    add_executable(test_speaker_vad
        tests/test_speaker_vad.cpp
    )
    target_link_libraries(test_speaker_vad PRIVATE
        livekit_signaling
    )

    add_executable(test_e2ee
        tests/test_e2ee.cpp
    )
    target_link_libraries(test_e2ee PRIVATE
        livekit_signaling
        ${OPENSSL_LIBRARIES}
    )
endif()

# Live Media Broadcaster Application
add_executable(livekit_media_broadcaster
    src/media/livekit_media_broadcaster.cpp
)
target_link_libraries(livekit_media_broadcaster PRIVATE
    livekit_signaling
)

# LiveKit SimpleRoom Standalone Executable
add_executable(simple_room
    src/app/simple_room.cpp
)
target_link_libraries(simple_room PRIVATE
    livekit_signaling
)

# LiveKit SimpleRpc Standalone Executable
add_executable(simple_rpc
    src/app/simple_rpc.cpp
)
target_link_libraries(simple_rpc PRIVATE
    livekit_signaling
)

# Lifecycle & Reconnect Stress Test (GAP-02)
add_executable(test_stress_lifecycle
    tests/test_stress_lifecycle.cpp
)
target_link_libraries(test_stress_lifecycle PRIVATE
    livekit_signaling
)

# Simulate Scenario Suite (Matching Flutter sendSimulateScenario)
add_executable(test_simulate_scenario
    tests/test_simulate_scenario.cpp
)
target_link_libraries(test_simulate_scenario PRIVATE
    livekit_signaling
)

# Multi-Codec Simulcast & Backup Codecs Test (GAP-03)
add_executable(test_backup_codecs
    tests/test_backup_codecs.cpp
)
target_link_libraries(test_backup_codecs PRIVATE
    livekit_signaling
)

# Multi-User Grid Math & Avatar Hash Test
add_executable(test_meeting_ui_grid
    tests/test_meeting_ui_grid.cpp
)



if(NOT DEFINED TDESKTOP_LIBS_DIR)
    if(DEFINED ENV{TDESKTOP_LIBS_DIR})
        set(TDESKTOP_LIBS_DIR "$ENV{TDESKTOP_LIBS_DIR}")
    else()
        set(TDESKTOP_LIBS_DIR "${DEPS_DIR}/Libraries/win64")
    endif()
endif()

set(QT_PATH "${TDESKTOP_LIBS_DIR}/Qt-5.15.18")
if(NOT EXISTS "${QT_PATH}")
    message(FATAL_ERROR "\n[ERROR] Qt 5.15.18 library not found in project dependencies: ${QT_PATH}\n"
                        "Please run 'win.bat' (or 'prepare.bat') first to deploy dependencies to ./deps!\n")
endif()

list(APPEND CMAKE_PREFIX_PATH "${QT_PATH}")
set(Qt5_DIR "${QT_PATH}/lib/cmake/Qt5" CACHE PATH "Qt5 Directory" FORCE)
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Svg Network)

foreach(_tool rcc moc uic qmake)
    if(TARGET Qt5::${_tool})
        set_property(TARGET Qt5::${_tool} PROPERTY IMPORTED_LOCATION_DEBUG "${QT_PATH}/bin/${_tool}.exe")
        set_property(TARGET Qt5::${_tool} PROPERTY IMPORTED_LOCATION_RELEASE "${QT_PATH}/bin/${_tool}.exe")
        set_property(TARGET Qt5::${_tool} PROPERTY IMPORTED_LOCATION "${QT_PATH}/bin/${_tool}.exe")
    endif()
endforeach()

set(CMAKE_AUTOMOC ON)

# Add Telegram UI Subproject directly as native C++ targets
add_subdirectory(src/ui/tdesktop)

add_executable(livekit_meeting_app
    src/app/main_meeting_app.cpp
    src/net/http_types.h
    src/net/openmeeting_http_client.h
    src/net/openmeeting_http_client.cpp
    src/net/session_manager.h
    src/net/session_manager.cpp
    src/ui/login_dialog.h
    src/ui/login_dialog.cpp
    src/ui/sidebar_widget.cpp
    src/ui/sidebar_widget.h
    src/ui/action_card_widget.cpp
    src/ui/action_card_widget.h
    src/ui/schedule_widget.cpp
    src/ui/schedule_widget.h
    src/ui/meeting_main_window.cpp
    src/ui/meeting_main_window.h
    src/ui/meeting_room_window.cpp
    src/ui/meeting_room_window.h
    src/ui/meeting_log_console.cpp
    src/ui/meeting_log_console.h
    src/ui/meeting_ui_integration.h
    src/core/meeting_coordinator.h
    src/core/meeting_coordinator.cpp
    src/render/qt_cpu_video_renderer.h
    src/render/qt_cpu_video_renderer.cpp
    src/render/video_render_session.h
    src/render/video_render_session.cpp
    src/ui/video_test_widget.cpp
    src/ui/video_test_widget.h
    src/ui/audio_visualizer_widget.cpp
    src/ui/audio_visualizer_widget.h
    src/ui/participants_list_model.h
    src/ui/participants_list_model.cpp
    src/ui/participant_item_delegate.h
    src/ui/participant_item_delegate.cpp
    src/ui/participants_sidebar_widget.h
    src/ui/participants_sidebar_widget.cpp
    src/ui/meeting_chat_sidebar_widget.h
    src/ui/meeting_chat_sidebar_widget.cpp
    src/ui/dx11/dx11_types.h
    src/ui/dx11/dx11_shaders.h
    src/ui/dx11/dx11_renderer.h
    src/ui/dx11/dx11_renderer.cpp
    src/ui/dx11/dx11_texture_pool.h
    src/ui/dx11/dx11_texture_pool.cpp
    src/ui/dx11/dx11_video_canvas.h
    src/ui/dx11/dx11_video_canvas.cpp
)

target_include_directories(livekit_meeting_app BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WEBRTC_ROOT}/include/third_party/libyuv/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tdesktop/lib_ui
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tdesktop/gen
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tdesktop/gen/styles
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tdesktop/lib_base
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tdesktop/lib_rpl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tdesktop/lib_crl
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/GSL/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/range-v3/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/expected/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/xxHash
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore/5.15.18/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui/5.15.18/QtGui
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtWidgets
    ${TDESKTOP_LIBS_DIR}/zlib/include
)

target_link_libraries(livekit_meeting_app PRIVATE
    desktop_lib_ui
    desktop_lib_base
    desktop_lib_crl
    desktop_lz4
    ${TDESKTOP_LIBS_DIR}/tg_angle/out/Release/tg_angle.lib
    ${TDESKTOP_LIBS_DIR}/zlib/Release/zlibstatic.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Gui.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Widgets.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Network.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Svg.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5FontDatabaseSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5EventDispatcherSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5ThemeSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5WindowsUIAutomationSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5AccessibilitySupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5DeviceDiscoverySupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5EdidSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtfreetype.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtharfbuzz.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtlibpng.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/platforms/qwindows.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/styles/qwindowsvistastyle.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qsvg.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qico.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qjpeg.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qgif.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qtga.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qtiff.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qwbmp.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qwebp.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/iconengines/qsvgicon.lib
    ${TDESKTOP_LIBS_DIR}/mozjpeg/release/jpeg-static.lib
    livekit_signaling
    Dwmapi
    UxTheme
    d3d9
    dxgi
    d3d11
    d3dcompiler
    dxguid
    netapi32
    userenv
    imm32
    winmm
    wtsapi32
    version
)

set_target_properties(livekit_meeting_app PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

target_compile_definitions(livekit_meeting_app PRIVATE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    CRL_USE_WINAPI
    ASIO_STANDALONE
    WEBRTC_WIN
    NDEBUG
    _ITERATOR_DEBUG_LEVEL=0
    $<$<CONFIG:Debug>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
    $<$<CONFIG:Debug>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>
)

target_link_options(livekit_meeting_app PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
)

enable_testing()
add_test(NAME signaling_tests COMMAND livekit_signaling_tests_run_v4)
add_test(NAME panic_guard_test COMMAND test_panic_guard)
add_test(NAME advanced_communication_test COMMAND test_advanced_communication)
add_test(NAME chunking_simulcast_test COMMAND test_chunking_simulcast)
add_test(NAME data_stream_assembler_test COMMAND test_data_stream_assembler)
add_test(NAME rpc_system_test COMMAND test_rpc_system)
add_test(NAME room_state_events_test COMMAND test_room_state_events)
add_test(NAME remote_publication_lifecycle_test COMMAND test_remote_publication_lifecycle)
add_test(NAME local_unpublish_transaction_test COMMAND test_local_unpublish_transaction)
add_test(NAME camera_switch_transaction_test COMMAND test_camera_switch_transaction)
add_test(NAME audio_render_reference_apm_test COMMAND test_audio_render_reference_apm)
add_test(NAME meeting_recovery_ux_test COMMAND test_meeting_recovery_ux)
add_test(NAME media_streaming_test COMMAND test_media_streaming)
add_test(NAME stats_system_test COMMAND test_stats_system)
add_test(NAME audio_playout_warmup_test COMMAND test_audio_playout_warmup)
add_test(NAME simulcast_test COMMAND test_simulcast)
add_test(NAME stress_lifecycle_test COMMAND test_stress_lifecycle)
add_test(NAME simulate_scenario_test COMMAND test_simulate_scenario)
add_test(NAME backup_codecs_test COMMAND test_backup_codecs)
add_test(NAME meeting_ui_grid_test COMMAND test_meeting_ui_grid)

if(LIVEKIT_BUILD_EXTENDED_TESTS)
    add_test(NAME apm_3a_test COMMAND test_apm_3a)
    add_test(NAME attributes_permissions_quality_test COMMAND test_attributes_permissions_quality)
    add_test(NAME adaptive_stream_test COMMAND test_adaptive_stream)
    add_test(NAME speaker_vad_test COMMAND test_speaker_vad)
    add_test(NAME e2ee_test COMMAND test_e2ee)
endif()

if(LIVEKIT_BUILD_HARDWARE_TESTS)
    add_test(NAME wasapi_capture_test COMMAND test_wasapi_capture)
    add_test(NAME dshow_capture_test COMMAND test_dshow_capture)
endif()

if(LIVEKIT_BUILD_EXTERNAL_TESTS)
    add_test(NAME livekit_official_connect_test COMMAND test_livekit_official_connect)
endif()

# OpenMeeting HTTP Client Test
add_executable(test_openmeeting_http
    tests/test_openmeeting_http.cpp
    src/net/openmeeting_http_client.cpp
    src/net/openmeeting_http_client.h
    src/net/session_manager.cpp
    src/net/session_manager.h
    src/net/http_types.h
)
target_include_directories(test_openmeeting_http BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${GEN_DIR}
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtNetwork
)
target_link_libraries(test_openmeeting_http PRIVATE
    livekit_signaling
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
add_test(NAME openmeeting_http_test COMMAND test_openmeeting_http)

# Coordinator session runtime serialization test
add_executable(test_meeting_session_runtime
    tests/test_meeting_session_runtime.cpp
    src/core/meeting_session_runtime.h
)
target_include_directories(test_meeting_session_runtime BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
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
target_link_options(test_meeting_session_runtime PRIVATE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>
)
set_target_properties(test_meeting_session_runtime PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME meeting_session_runtime_test COMMAND test_meeting_session_runtime)

# Coordinator startup transaction test: a meeting is committed only after both
# required local tracks publish, otherwise it must roll back to failure.
add_executable(test_meeting_startup_transaction
    tests/test_meeting_startup_transaction.cpp
    src/core/meeting_startup_transaction.h
)
target_include_directories(test_meeting_startup_transaction BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
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

# GPU Video Converter Test
add_executable(test_gpu_converter
    tests/test_gpu_converter.cpp
    src/ui/dx11/dx11_types.h
    src/ui/dx11/dx11_shaders.h
    src/ui/dx11/gpu_video_converter.h
    src/ui/dx11/gpu_video_converter.cpp
)

target_include_directories(test_gpu_converter BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
)

target_link_libraries(test_gpu_converter PRIVATE
    livekit_signaling
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
    tests/test_dx11_renderer_lifecycle.cpp
    src/ui/dx11/dx11_types.h
    src/ui/dx11/dx11_color_conversion.h
    src/ui/dx11/dx11_shaders.h
    src/ui/dx11/dx11_renderer.h
    src/ui/dx11/dx11_renderer.cpp
)

target_include_directories(test_dx11_renderer_lifecycle BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
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
    tests/test_dx11_color_conversion.cpp
    src/ui/dx11/dx11_color_conversion.h
)

target_include_directories(test_dx11_color_conversion BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(test_dx11_color_conversion PRIVATE
    livekit_signaling
)

set_target_properties(test_dx11_color_conversion PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME dx11_color_conversion_test COMMAND test_dx11_color_conversion)

# Compile the embedded HLSL without creating a device or native window. This
# catches shader/register-layout regressions in CI and remote sessions.
add_executable(test_dx11_shaders
    tests/test_dx11_shaders.cpp
    src/ui/dx11/dx11_shaders.h
)

target_include_directories(test_dx11_shaders BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
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
    tests/test_owned_i420_frame.cpp
)

target_link_libraries(test_owned_i420_frame PRIVATE
    livekit_signaling
)

set_target_properties(test_owned_i420_frame PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME owned_i420_frame_test COMMAND test_owned_i420_frame)

add_executable(test_video_render_router
    tests/test_video_render_router.cpp
)

target_link_libraries(test_video_render_router PRIVATE
    livekit_signaling
)

set_target_properties(test_video_render_router PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
add_test(NAME video_render_router_test COMMAND test_video_render_router)

add_executable(test_qt_cpu_video_renderer
    tests/test_qt_cpu_video_renderer.cpp
    src/render/qt_cpu_video_renderer.h
    src/render/qt_cpu_video_renderer.cpp
    src/render/video_render_session.h
    src/render/video_render_session.cpp
)

target_include_directories(test_qt_cpu_video_renderer BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WEBRTC_ROOT}/include/third_party/libyuv/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
)

target_link_libraries(test_qt_cpu_video_renderer PRIVATE
    livekit_signaling
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
    tests/test_video_render_session_stress.cpp
    src/render/qt_cpu_video_renderer.h
    src/render/qt_cpu_video_renderer.cpp
    src/render/video_render_session.h
    src/render/video_render_session.cpp
)

target_include_directories(test_video_render_session_stress BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WEBRTC_ROOT}/include/third_party/libyuv/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
)

target_link_libraries(test_video_render_session_stress PRIVATE
    livekit_signaling
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


