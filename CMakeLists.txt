cmake_minimum_required(VERSION 3.20)
project(livekit_signaling_cpp LANGUAGES C CXX)

option(LIVEKIT_BUILD_SAMPLES "Build standalone sample applications" ON)
option(LIVEKIT_BUILD_MEETING_APP "Build the Qt meeting application" ON)
option(BUILD_TESTING "Build the test targets" ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
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
    src/signal/credential_url.cpp
    src/signal/websocket_client.cpp
    src/signal/region_provider.cpp
    src/signal/signal_stream.cpp
    src/signal/signal_client.cpp
    src/core/participant.cpp
    src/core/room.cpp
    src/core/local_audio_track.cpp
    src/core/local_video_track.cpp
    src/core/screen_share_session.cpp
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
    src/render/video_render_frame.h
    src/render/video_render_frame.cpp
    src/render/video_render_router.cpp
    src/media/media_converters.cpp
    src/media/wasapi_enumerator.cpp
    src/media/wasapi_capture.cpp
    src/media/dshow_enumerator.cpp
    src/media/dshow_capture.cpp
    src/media/desktop_capture.cpp
    src/media/camera_source_manager.cpp
    src/media/audio_vad.cpp
    src/media/audio_apm.cpp
    src/e2ee/key_provider.cpp
    src/e2ee/frame_cryptor.cpp
    src/telemetry/crash_handler.cpp
    src/telemetry/log_redaction.cpp
    src/telemetry/stats_collector.cpp
)

if(MSVC)
    # Room's coroutine-heavy state machine exceeds the default COFF section
    # count once the subscription owner protocol is included.
    set_source_files_properties(src/core/room.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
    # The bundled Windows WebRTC includes WGC. This macro changes the layout
    # of DesktopCaptureOptions and must match its compiled library ABI.
    set_source_files_properties(src/media/desktop_capture.cpp PROPERTIES
        COMPILE_DEFINITIONS "RTC_ENABLE_WIN_WGC")
endif()

# 头文件列表
set(LIVEKIT_HEADERS
    src/media/audio_apm.h
    src/signal/credential_url.h
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
    ${WEBRTC_ROOT}/include/third_party/libyuv/include
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
    d3d11
    dxgi
    dwmapi
    shcore
)

if(LIVEKIT_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(LIVEKIT_BUILD_MEETING_APP OR BUILD_TESTING)
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
file(GLOB livekit_qt_package_dirs LIST_DIRECTORIES TRUE
    "${QT_PATH}/lib/cmake/Qt5*")
foreach(livekit_qt_package_dir IN LISTS livekit_qt_package_dirs)
    if(IS_DIRECTORY "${livekit_qt_package_dir}")
        get_filename_component(livekit_qt_package "${livekit_qt_package_dir}" NAME)
        set(${livekit_qt_package}_DIR "${livekit_qt_package_dir}"
            CACHE PATH "${livekit_qt_package} directory" FORCE)
    endif()
endforeach()
find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Svg Network)

# The bundled Qt libraries are release-only and the whole project uses the
# matching release CRT ABI in every CMake configuration. Qt 5's static package
# embeds configuration expressions in its link interface, so mapping the
# imported location alone would still pull debug support libraries into Debug.
get_property(livekit_imported_targets DIRECTORY PROPERTY IMPORTED_TARGETS)
foreach(livekit_imported_target IN LISTS livekit_imported_targets)
    if(livekit_imported_target MATCHES "^Qt5::")
        set_property(TARGET ${livekit_imported_target}
            PROPERTY MAP_IMPORTED_CONFIG_DEBUG Release)

        foreach(livekit_imported_property IN ITEMS LOCATION IMPLIB SONAME)
            get_target_property(livekit_qt_release_value
                ${livekit_imported_target}
                IMPORTED_${livekit_imported_property}_RELEASE)
            if(livekit_qt_release_value
                    AND NOT livekit_qt_release_value MATCHES "-NOTFOUND$")
                set_property(TARGET ${livekit_imported_target}
                    PROPERTY IMPORTED_${livekit_imported_property}_DEBUG
                    "${livekit_qt_release_value}")
            endif()
        endforeach()

        get_target_property(livekit_qt_release_dependencies
            ${livekit_imported_target}
            IMPORTED_LINK_INTERFACE_LIBRARIES_RELEASE)
        if(livekit_qt_release_dependencies
                AND NOT livekit_qt_release_dependencies MATCHES "-NOTFOUND$")
            set_property(TARGET ${livekit_imported_target}
                PROPERTY INTERFACE_LINK_LIBRARIES
                "${livekit_qt_release_dependencies}")
        endif()
    endif()
endforeach()

foreach(_tool rcc moc uic qmake)
    if(TARGET Qt5::${_tool})
        set_property(TARGET Qt5::${_tool} PROPERTY IMPORTED_LOCATION_DEBUG "${QT_PATH}/bin/${_tool}.exe")
        set_property(TARGET Qt5::${_tool} PROPERTY IMPORTED_LOCATION_RELEASE "${QT_PATH}/bin/${_tool}.exe")
        set_property(TARGET Qt5::${_tool} PROPERTY IMPORTED_LOCATION "${QT_PATH}/bin/${_tool}.exe")
    endif()
endforeach()

set(CMAKE_AUTOMOC ON)

# Add UI support libraries and generated resources.
add_subdirectory(src/ui)

add_subdirectory(src/render)

if(LIVEKIT_BUILD_MEETING_APP)
    add_subdirectory(src/app)
endif()
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
