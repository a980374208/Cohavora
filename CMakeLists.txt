cmake_minimum_required(VERSION 3.20)
project(livekit_signaling_cpp LANGUAGES C CXX)

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
)
set(PROTO_HDRS
    ${GEN_DIR}/logger/options.pb.h
    ${GEN_DIR}/livekit_metrics.pb.h
    ${GEN_DIR}/livekit_models.pb.h
    ${GEN_DIR}/livekit_rtc.pb.h
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
    src/rtc/webrtc_manager.cpp
    src/rtc/audio_frame.cpp
    src/rtc/video_frame.cpp
    src/rtc/audio_source.cpp
    src/rtc/video_source.cpp
    src/rtc/audio_stream.cpp
    src/rtc/video_stream.cpp
    src/rtc/rtc_audio_source.cpp
    src/rtc/rtc_video_source.cpp
    src/media/media_converters.cpp
    src/media/wasapi_enumerator.cpp
    src/media/wasapi_capture.cpp
    src/media/dshow_enumerator.cpp
    src/media/dshow_capture.cpp
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
    src/core/participant.h
    src/core/track.h
    src/core/local_audio_track.h
    src/core/local_video_track.h
    src/core/remote_track_publication.h
    src/core/adaptive_stream_manager.h
    src/rtc/webrtc_manager.h
    src/rtc/audio_frame.h
    src/rtc/video_frame.h
    src/rtc/audio_source.h
    src/rtc/video_source.h
    src/rtc/audio_stream.h
    src/rtc/video_stream.h
    src/rtc/rtc_audio_source.h
    src/rtc/rtc_video_source.h
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

# APM 3A Audio Processor Test
add_executable(test_apm_3a
    tests/test_apm_3a.cpp
)
target_link_libraries(test_apm_3a PRIVATE
    livekit_signaling
)

# Participant Attributes, Permissions & Track Quality Control Test
add_executable(test_attributes_permissions_quality
    tests/test_attributes_permissions_quality.cpp
)
target_link_libraries(test_attributes_permissions_quality PRIVATE
    livekit_signaling
)

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

# Official Server Connect Test
add_executable(test_livekit_official_connect
    tests/test_livekit_official_connect.cpp
)
target_link_libraries(test_livekit_official_connect PRIVATE
    livekit_signaling
)

# WASAPI Audio Capture Test
add_executable(test_wasapi_capture
    tests/test_wasapi_capture.cpp
)
target_link_libraries(test_wasapi_capture PRIVATE
    livekit_signaling
)

# DirectShow Video Capture Test
add_executable(test_dshow_capture
    tests/test_dshow_capture.cpp
)
target_link_libraries(test_dshow_capture PRIVATE
    livekit_signaling
)

# Media Pipeline E2E Test
add_executable(test_media_pipeline_e2e
    tests/test_media_pipeline_e2e.cpp
)
target_link_libraries(test_media_pipeline_e2e PRIVATE
    livekit_signaling
)

# VP8 Simulcast Test
add_executable(test_simulcast
    tests/test_simulcast.cpp
)
target_link_libraries(test_simulcast PRIVATE
    livekit_signaling
)

# Adaptive Stream Test
add_executable(test_adaptive_stream
    tests/test_adaptive_stream.cpp
)
target_link_libraries(test_adaptive_stream PRIVATE
    livekit_signaling
)

# Network Emulation Benchmark Test
add_executable(test_network_emulation_benchmark
    tests/test_network_emulation_benchmark.cpp
)
target_link_libraries(test_network_emulation_benchmark PRIVATE
    livekit_signaling
)

# Speaker VAD Test
add_executable(test_speaker_vad
    tests/test_speaker_vad.cpp
)
target_link_libraries(test_speaker_vad PRIVATE
    livekit_signaling
)

# E2EE Test
add_executable(test_e2ee
    tests/test_e2ee.cpp
)
target_link_libraries(test_e2ee PRIVATE
    livekit_signaling
    ${OPENSSL_LIBRARIES}
)

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
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Svg)

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
    src/ui/video_test_widget.cpp
    src/ui/video_test_widget.h
    src/ui/audio_visualizer_widget.cpp
    src/ui/audio_visualizer_widget.h
)

target_include_directories(livekit_meeting_app BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
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
add_test(NAME rpc_system_test COMMAND test_rpc_system)
add_test(NAME media_streaming_test COMMAND test_media_streaming)
add_test(NAME stats_system_test COMMAND test_stats_system)
add_test(NAME livekit_official_connect_test COMMAND test_livekit_official_connect)
add_test(NAME wasapi_capture_test COMMAND test_wasapi_capture)
add_test(NAME dshow_capture_test COMMAND test_dshow_capture)
add_test(NAME media_pipeline_e2e_test COMMAND test_media_pipeline_e2e)
add_test(NAME simulcast_test COMMAND test_simulcast)
add_test(NAME network_emulation_benchmark COMMAND test_network_emulation_benchmark)



