cmake_minimum_required(VERSION 3.20)
project(livekit_signaling_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "" FORCE)

set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG "Release;None")

# Find Packages via CMake / Vcpkg
find_package(asio CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

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

# Main Signaling Library
add_library(livekit_signaling STATIC
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    src/websocket_client.cpp
    src/region_provider.cpp
    src/signal_stream.cpp
    src/signal_client.cpp
    src/participant.cpp
    src/room.cpp
    src/webrtc_manager.cpp
    src/crash_handler.cpp
    src/audio_frame.cpp
    src/video_frame.cpp
    src/audio_source.cpp
    src/video_source.cpp
    src/audio_stream.cpp
    src/video_stream.cpp
    src/local_audio_track.cpp
    src/local_video_track.cpp
    src/rtc_audio_source.cpp
    src/rtc_video_source.cpp
    src/stats_collector.cpp
)

set(WEBRTC_ROOT "E:/vsSource/WebRTC/client-sdk-cpp/client-sdk-rust/target/debug/build/scratch-f4ffd3e87af6f636/out/livekit_webrtc/livekit/win-x64-release-webrtc-51ef663/win-x64-release")

target_include_directories(livekit_signaling PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GEN_DIR}
    ${WEBRTC_ROOT}/include
)

target_compile_definitions(livekit_signaling PUBLIC
    WEBRTC_WIN
    NOMINMAX
    NDEBUG
    DCHECK_ALWAYS_ON=0
    RTC_DISABLE_CHECK_MSG=1
    RTC_DISABLE_CHECKING=1
    $<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>
)

target_link_libraries(livekit_signaling PUBLIC
    asio::asio
    OpenSSL::SSL
    OpenSSL::Crypto
    protobuf::libprotobuf
    spdlog::spdlog
    ZLIB::ZLIB
    nlohmann_json::nlohmann_json
    ${WEBRTC_ROOT}/lib/webrtc.lib
    ws2_32
    winmm
    secur32
    crypt32
    iphlpapi
    dmoguids
    msdmo
    amstrmid
    wmcodecdspuuid
)

# Unit/Integration Tests
add_executable(livekit_signaling_tests_run_v4
    tests/main.cpp
)

target_link_libraries(livekit_signaling_tests_run_v4 PRIVATE
    livekit_signaling
)
target_link_options(livekit_signaling_tests_run_v4 PRIVATE
    "/FORCE:MULTIPLE"
)

# Panic Guard Test
add_executable(test_panic_guard
    tests/test_panic_guard.cpp
)
target_link_libraries(test_panic_guard PRIVATE
    livekit_signaling
)
target_link_options(test_panic_guard PRIVATE
    "/FORCE:MULTIPLE"
)

# Advanced Communication Test
add_executable(test_advanced_communication
    tests/test_advanced_communication.cpp
)
target_link_libraries(test_advanced_communication PRIVATE
    livekit_signaling
)
target_link_options(test_advanced_communication PRIVATE
    "/FORCE:MULTIPLE"
)

# RPC System Test
add_executable(test_rpc_system
    tests/test_rpc_system.cpp
)
target_link_libraries(test_rpc_system PRIVATE
    livekit_signaling
)
target_link_options(test_rpc_system PRIVATE
    "/FORCE:MULTIPLE"
)

# Media Streaming Test
add_executable(test_media_streaming
    tests/test_media_streaming.cpp
)
target_link_libraries(test_media_streaming PRIVATE
    livekit_signaling
)
target_link_options(test_media_streaming PRIVATE
    "/FORCE:MULTIPLE"
)

# Stats System Test
add_executable(test_stats_system
    tests/test_stats_system.cpp
)
target_link_libraries(test_stats_system PRIVATE
    livekit_signaling
)
target_link_options(test_stats_system PRIVATE
    "/FORCE:MULTIPLE"
)

enable_testing()
add_test(NAME signaling_tests COMMAND livekit_signaling_tests_run_v4)
add_test(NAME panic_guard_test COMMAND test_panic_guard)
add_test(NAME advanced_communication_test COMMAND test_advanced_communication)
add_test(NAME rpc_system_test COMMAND test_rpc_system)
add_test(NAME media_streaming_test COMMAND test_media_streaming)
add_test(NAME stats_system_test COMMAND test_stats_system)


