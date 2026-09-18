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
    src/core/data_stream.cpp
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
    src/telemetry/log_redaction.cpp
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
    src/telemetry/log_redaction.h
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
    src/net/credential_store.cpp
    src/net/credential_store.h
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
    src/ui/camera_switch_completion_owner.cpp
    src/ui/camera_switch_completion_owner.h
    src/ui/meeting_log_console.cpp
    src/ui/meeting_log_console.h
    src/ui/meeting_ui_integration.h
    src/core/meeting_coordinator.h
    src/core/meeting_coordinator.cpp
    src/render/qt_cpu_video_renderer.h
    src/render/qt_cpu_video_renderer.cpp
    src/render/video_render_session.h
    src/render/video_render_session.cpp
    src/ui/video_preview_widget.cpp
    src/ui/video_preview_widget.h
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
