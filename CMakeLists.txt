cmake_minimum_required(VERSION 3.25)

# Select MSVC debug information before enabling C/C++ so CMake does not inject
# legacy flags into every configuration. Debug keeps cache-friendly embedded
# symbols, RelWithDebInfo emits a PDB, and release configurations stay lean.
if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<$<CONFIG:Debug>:Embedded>$<$<CONFIG:RelWithDebInfo>:ProgramDatabase>")
endif()

# A vcpkg manifest is activated only when its toolchain is loaded before the
# first project() call. Keep explicit toolchains authoritative, and make the
# documented bare configure command work when VCPKG_ROOT is available.
if(CMAKE_HOST_WIN32)
    set(COHAVORA_DEPENDENCY_ARCHITECTURE "x64" CACHE STRING
        "Architecture of the bundled WebRTC and Qt dependency set")
    set_property(CACHE COHAVORA_DEPENDENCY_ARCHITECTURE PROPERTY STRINGS x64)

    set(cohavora_requested_architecture "${CMAKE_GENERATOR_PLATFORM}")
    if(cohavora_requested_architecture STREQUAL ""
            AND DEFINED ENV{VSCMD_ARG_TGT_ARCH})
        set(cohavora_requested_architecture "$ENV{VSCMD_ARG_TGT_ARCH}")
    endif()
    if(cohavora_requested_architecture STREQUAL "")
        set(cohavora_requested_architecture "x64")
    endif()
    string(TOLOWER "${cohavora_requested_architecture}"
        cohavora_requested_architecture)
    if(cohavora_requested_architecture MATCHES "^(amd64|x86_64)$")
        set(cohavora_requested_architecture "x64")
    endif()
    if(NOT cohavora_requested_architecture STREQUAL
            COHAVORA_DEPENDENCY_ARCHITECTURE)
        message(FATAL_ERROR
            "The bundled WebRTC and Qt dependencies support x64 only; "
            "requested generator/toolchain architecture is "
            "'${cohavora_requested_architecture}'.")
    endif()

    if(NOT DEFINED VCPKG_TARGET_TRIPLET OR VCPKG_TARGET_TRIPLET STREQUAL "")
        set(VCPKG_TARGET_TRIPLET "x64-windows-static"
            CACHE STRING "vcpkg target triplet")
    elseif(NOT VCPKG_TARGET_TRIPLET MATCHES "^x64-")
        message(FATAL_ERROR
            "VCPKG_TARGET_TRIPLET='${VCPKG_TARGET_TRIPLET}' does not match "
            "the x64 WebRTC and Qt dependency set.")
    endif()
    unset(cohavora_requested_architecture)
endif()

if((NOT DEFINED CMAKE_TOOLCHAIN_FILE OR CMAKE_TOOLCHAIN_FILE STREQUAL "")
        AND DEFINED ENV{VCPKG_ROOT})
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" cohavora_vcpkg_root)
    set(cohavora_vcpkg_toolchain
        "${cohavora_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
    if(EXISTS "${cohavora_vcpkg_toolchain}")
        set(CMAKE_TOOLCHAIN_FILE "${cohavora_vcpkg_toolchain}"
            CACHE FILEPATH "CMake toolchain file" FORCE)
    endif()
    unset(cohavora_vcpkg_toolchain)
    unset(cohavora_vcpkg_root)
endif()

project(Cohavora VERSION 0.1.0
    DESCRIPTION "Cohavora - Open-source Audio and Video Meeting Client"
    LANGUAGES C CXX)
set(COHAVORA_DISPLAY_NAME "Cohavora · 开源音视频会议客户端")

if(CMAKE_HOST_WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR
            "The selected compiler is not targeting the required 64-bit ABI.")
    endif()
    if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID
            AND NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^(x64|X64|AMD64)$")
        message(FATAL_ERROR
            "Compiler architecture '${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}' "
            "does not match the x64 WebRTC and Qt dependency set.")
    endif()
endif()

function(cohavora_compat_option current legacy description default_value)
    if(DEFINED CACHE{${current}})
        set(initial_value "${${current}}")
    elseif(DEFINED CACHE{${legacy}})
        set(initial_value "${${legacy}}")
    else()
        set(initial_value "${default_value}")
    endif()
    if(DEFINED CACHE{${legacy}})
        message(DEPRECATION
            "${legacy} is deprecated; use ${current}. ${current} takes precedence when both are set.")
    endif()
    option(${current} "${description}" "${initial_value}")
endfunction()

cohavora_compat_option(COHAVORA_BUILD_SAMPLES LIVEKIT_BUILD_SAMPLES
    "Build standalone sample applications" ON)
cohavora_compat_option(COHAVORA_BUILD_MEETING_APP LIVEKIT_BUILD_MEETING_APP
    "Build the Qt meeting application" ON)
option(BUILD_TESTING "Build the test targets" ON)
option(COHAVORA_BUILD_QT_TESTS
    "Build the test groups that require the Qt/UI dependency stack" ON)
option(COHAVORA_BUILD_RUNTIME_TOOLS
    "Create manual runtime harness targets (excluded from ALL_BUILD)" OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/CohavoraAbi.cmake)

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
    set(PROTOC_DEPENDENCY protobuf::protoc)
else()
    find_program(PROTOC_EXE NAMES protoc REQUIRED)
    set(PROTOC_DEPENDENCY "${PROTOC_EXE}")
endif()

# Track well-known schemas as file dependencies and pass their roots explicitly
# to protoc. A tool or imported-schema update must invalidate generated output.
set(PROTOBUF_IMPORT_CANDIDATES ${Protobuf_INCLUDE_DIRS})
if(TARGET protobuf::libprotobuf)
    get_target_property(PROTOBUF_TARGET_INCLUDE_DIRS
        protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
    if(PROTOBUF_TARGET_INCLUDE_DIRS
            AND NOT PROTOBUF_TARGET_INCLUDE_DIRS MATCHES "-NOTFOUND$")
        list(APPEND PROTOBUF_IMPORT_CANDIDATES ${PROTOBUF_TARGET_INCLUDE_DIRS})
    endif()
endif()

set(PROTOBUF_IMPORT_DIRS)
set(PROTOBUF_IMPORT_FILES)
foreach(PROTOBUF_IMPORT_CANDIDATE IN LISTS PROTOBUF_IMPORT_CANDIDATES)
    if(PROTOBUF_IMPORT_CANDIDATE MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
        set(PROTOBUF_IMPORT_CANDIDATE "${CMAKE_MATCH_1}")
    endif()
    if(EXISTS "${PROTOBUF_IMPORT_CANDIDATE}/google/protobuf/descriptor.proto"
            AND EXISTS "${PROTOBUF_IMPORT_CANDIDATE}/google/protobuf/timestamp.proto")
        list(APPEND PROTOBUF_IMPORT_DIRS "${PROTOBUF_IMPORT_CANDIDATE}")
        list(APPEND PROTOBUF_IMPORT_FILES
            "${PROTOBUF_IMPORT_CANDIDATE}/google/protobuf/descriptor.proto"
            "${PROTOBUF_IMPORT_CANDIDATE}/google/protobuf/timestamp.proto")
    endif()
endforeach()
list(REMOVE_DUPLICATES PROTOBUF_IMPORT_DIRS)
list(REMOVE_DUPLICATES PROTOBUF_IMPORT_FILES)
if(NOT PROTOBUF_IMPORT_FILES)
    message(FATAL_ERROR
        "Protobuf well-known schemas descriptor.proto and timestamp.proto were not found")
endif()

set(PROTOC_IMPORT_ARGUMENTS)
foreach(PROTOBUF_IMPORT_DIR IN LISTS PROTOBUF_IMPORT_DIRS)
    list(APPEND PROTOC_IMPORT_ARGUMENTS "--proto_path=${PROTOBUF_IMPORT_DIR}")
endforeach()

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
        ${PROTOC_IMPORT_ARGUMENTS}
        --cpp_out=${GEN_DIR}
        ${PROTO_FILES}
    DEPENDS ${PROTO_FILES} ${PROTOBUF_IMPORT_FILES} ${PROTOC_DEPENDENCY}
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
    src/core/publication_catalog.cpp
    src/core/video_demand_policy.cpp
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
    src/telemetry/e2e_media_marker.cpp
    src/telemetry/e2e_measurement.cpp
    src/telemetry/log_redaction.cpp
    src/telemetry/process_resource_sampler.cpp
    src/telemetry/stability_ledger.cpp
    src/telemetry/stats_collector.cpp
    src/telemetry/session_telemetry.cpp
    src/telemetry/telemetry_report.cpp
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
    src/core/publication_catalog.h
    src/core/video_demand_policy.h
    src/core/video_demand_types.h
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
    src/telemetry/session_telemetry.h
    src/telemetry/telemetry.h
    src/telemetry/crash_handler.h
    src/telemetry/e2e_media_marker.h
    src/telemetry/e2e_measurement.h
    src/telemetry/log_redaction.h
    src/telemetry/process_resource_sampler.h
    src/telemetry/stability_ledger.h
    src/telemetry/telemetry_report.h
    src/telemetry/safe_spawn.h
    src/telemetry/coro_allocator.h
)

# 源文件分组
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Source Files" FILES ${LIVEKIT_SOURCES})

# 头文件分组
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" PREFIX "Header Files" FILES ${LIVEKIT_HEADERS})

# Keep generated protobuf sources together in Visual Studio while preserving
# subdirectories such as logger/ beneath the generated output root.
source_group(TREE "${GEN_DIR}" PREFIX "Generated Files\\Protobuf"
    FILES ${PROTO_SRCS} ${PROTO_HDRS})

# Main Signaling Library
add_library(cohavora_core STATIC
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

target_include_directories(cohavora_core BEFORE PUBLIC
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

set(WEBRTC_REQUIRED_PATHS
    "${WEBRTC_ROOT}/include/api/peer_connection_interface.h"
    "${WEBRTC_ROOT}/include/api/video/i420_buffer.h"
    "${WEBRTC_ROOT}/include/third_party/libyuv/include/libyuv.h"
    "${WEBRTC_ROOT}/include/third_party/boringssl/src/include/openssl/ssl.h"
    "${WEBRTC_DEBUG_LIB}"
    "${WEBRTC_RELEASE_LIB}")
set(WEBRTC_MISSING_PATHS)
foreach(WEBRTC_REQUIRED_PATH IN LISTS WEBRTC_REQUIRED_PATHS)
    if(NOT EXISTS "${WEBRTC_REQUIRED_PATH}")
        list(APPEND WEBRTC_MISSING_PATHS "${WEBRTC_REQUIRED_PATH}")
    endif()
endforeach()
if(WEBRTC_MISSING_PATHS)
    list(JOIN WEBRTC_MISSING_PATHS "\n  - " WEBRTC_MISSING_DETAILS)
    message(FATAL_ERROR
        "The selected WebRTC SDK is incomplete:\n  - ${WEBRTC_MISSING_DETAILS}")
endif()

message(STATUS "[WebRTC] Using Debug Library   : ${WEBRTC_DEBUG_LIB}")
message(STATUS "[WebRTC] Using Release Library : ${WEBRTC_RELEASE_LIB}")

# The packaged WebRTC archive contains BoringSSL. All OpenSSL-compatible code
# in a process that links WebRTC must use those headers and symbols; linking a
# second static OpenSSL implementation produces thousands of duplicate symbols
# and leaves the selected implementation dependent on link order.
add_library(cohavora_webrtc_sdk INTERFACE)
target_include_directories(cohavora_webrtc_sdk SYSTEM BEFORE INTERFACE
    "${WEBRTC_ROOT}/include/third_party/boringssl/src/include"
    "${WEBRTC_ROOT}/include"
    "${WEBRTC_ROOT}/include/third_party/libyuv/include")
target_link_libraries(cohavora_webrtc_sdk INTERFACE
    $<$<CONFIG:Debug>:${WEBRTC_DEBUG_LIB}>
    $<$<NOT:$<CONFIG:Debug>>:${WEBRTC_RELEASE_LIB}>)

# Static QtNetwork requires OpenSSL APIs that WebRTC's bundled BoringSSL does
# not provide. Only final binaries that intentionally combine both providers
# inherit this MSVC compatibility switch; unrelated targets retain duplicate
# symbol diagnostics.
add_library(cohavora_dual_tls_link_compat INTERFACE)
if(MSVC)
    target_link_options(cohavora_dual_tls_link_compat INTERFACE /FORCE:MULTIPLE)
endif()

add_compile_definitions(
    WEBRTC_WIN
    NOMINMAX
    RTC_DISABLE_CHECK_MSG=1
    RTC_DISABLE_CHECKING=1
    RTC_NO_UNIQUE_ADDRESS=
)

target_link_libraries(cohavora_core PUBLIC
    cohavora_webrtc_sdk
    asio::asio
    protobuf::libprotobuf
    spdlog::spdlog
    ZLIB::ZLIB
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

if(COHAVORA_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(COHAVORA_BUILD_MEETING_APP OR (BUILD_TESTING AND COHAVORA_BUILD_QT_TESTS))
if(NOT DEFINED TDESKTOP_LIBS_DIR)
    if(DEFINED ENV{TDESKTOP_LIBS_DIR})
        set(TDESKTOP_LIBS_DIR "$ENV{TDESKTOP_LIBS_DIR}")
    else()
        set(TDESKTOP_LIBS_DIR "${DEPS_DIR}/Libraries/win64")
    endif()
endif()

set(QT_PATH "${TDESKTOP_LIBS_DIR}/Qt-5.15.18")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/build/prepare/libraries-required.txt"
    COHAVORA_LIBRARIES_REQUIRED_FILES REGEX "^[^#].+")
set(COHAVORA_LIBRARIES_MISSING_PATHS)
foreach(COHAVORA_LIBRARY_REQUIRED_FILE IN LISTS COHAVORA_LIBRARIES_REQUIRED_FILES)
    if(NOT EXISTS "${TDESKTOP_LIBS_DIR}/${COHAVORA_LIBRARY_REQUIRED_FILE}")
        list(APPEND COHAVORA_LIBRARIES_MISSING_PATHS
            "${TDESKTOP_LIBS_DIR}/${COHAVORA_LIBRARY_REQUIRED_FILE}")
    endif()
endforeach()
if(COHAVORA_LIBRARIES_MISSING_PATHS)
    list(JOIN COHAVORA_LIBRARIES_MISSING_PATHS
        "\n  - " COHAVORA_LIBRARIES_MISSING_DETAILS)
    message(FATAL_ERROR
        "The selected Qt/third-party dependency tree is incomplete:\n"
        "  - ${COHAVORA_LIBRARIES_MISSING_DETAILS}\n"
        "Run build/prepare/win.bat with --qt-archive or --libraries-src to repair it.")
endif()

include(cmake/CohavoraQtDependencies.cmake)
cohavora_configure_qt_dependencies(
    QT_ROOT "${QT_PATH}"
    LIBRARIES_ROOT "${TDESKTOP_LIBS_DIR}")

# Add UI support libraries and generated resources.
add_subdirectory(src/ui)
cohavora_define_desktop_ui_runtime()

include(cmake/CohavoraMeetingTargets.cmake)

add_subdirectory(src/render)

include(cmake/CohavoraMeetingUiTargets.cmake)

if(COHAVORA_BUILD_MEETING_APP)
    add_subdirectory(src/app)
endif()
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TARGET cohavora_app)
    include(cmake/CohavoraPackaging.cmake)
    if(CMAKE_GENERATOR MATCHES "^Visual Studio")
        set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY
            VS_STARTUP_PROJECT cohavora_app)
    endif()
endif()

# AUTOMOC aggregation sources must remain compiled, but they are generated
# implementation details and should not clutter Visual Studio projects.
if(CMAKE_GENERATOR MATCHES "^Visual Studio")
    function(cohavora_hide_automoc_sources source_dir)
        get_property(directory_targets DIRECTORY "${source_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS directory_targets)
            get_target_property(target_automoc ${target} AUTOMOC)
            if(target_automoc)
                get_target_property(target_binary_dir ${target} BINARY_DIR)
                foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
                    set_source_files_properties(
                        "${target_binary_dir}/${target}_autogen/mocs_compilation_${config}.cpp"
                        TARGET_DIRECTORY ${target}
                        PROPERTIES VS_SETTINGS "Visible=false")
                endforeach()
            endif()
        endforeach()

        get_property(subdirectories DIRECTORY "${source_dir}" PROPERTY SUBDIRECTORIES)
        foreach(subdirectory IN LISTS subdirectories)
            cohavora_hide_automoc_sources("${subdirectory}")
        endforeach()
    endfunction()

    cohavora_hide_automoc_sources("${CMAKE_CURRENT_SOURCE_DIR}")
endif()
