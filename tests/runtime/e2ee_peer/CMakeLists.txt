cmake_minimum_required(VERSION 3.20)
project(livekit_e2ee_interop_peer LANGUAGES CXX)

# Consume the existing official SDK build without configuring/building Reference.
set(LIVEKIT_CPP_ROOT "" CACHE PATH "Read-only official client-sdk-cpp checkout")
set(LIVEKIT_CPP_BUILD "" CACHE PATH "Existing matching Debug SDK build")
if(NOT EXISTS "${LIVEKIT_CPP_BUILD}/lib/livekit.lib" OR
   NOT EXISTS "${LIVEKIT_CPP_ROOT}/include/livekit/room.h")
    message(FATAL_ERROR "An existing official Windows Debug SDK build is required")
endif()
add_library(official_livekit SHARED IMPORTED)
set_target_properties(official_livekit PROPERTIES
    IMPORTED_IMPLIB "${LIVEKIT_CPP_BUILD}/lib/livekit.lib"
    IMPORTED_LOCATION "${LIVEKIT_CPP_BUILD}/bin/livekit.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${LIVEKIT_CPP_ROOT}/include")
add_executable(test_e2ee_official_sender sender.cpp)
target_compile_features(test_e2ee_official_sender PRIVATE cxx_std_20)
set_property(TARGET test_e2ee_official_sender PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDebugDLL)
target_compile_options(test_e2ee_official_sender PRIVATE /utf-8)
target_link_libraries(test_e2ee_official_sender PRIVATE official_livekit)
add_executable(test_e2ee_official_receiver receiver.cpp)
target_compile_features(test_e2ee_official_receiver PRIVATE cxx_std_20)
set_property(TARGET test_e2ee_official_receiver PROPERTY MSVC_RUNTIME_LIBRARY MultiThreadedDebugDLL)
target_compile_options(test_e2ee_official_receiver PRIVATE /utf-8)
target_link_libraries(test_e2ee_official_receiver PRIVATE official_livekit bcrypt)
