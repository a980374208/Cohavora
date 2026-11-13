# Keep the separately loaded HEVC decoder isolated from WebRTC's static FFmpeg.
set(COHAVORA_H265_RUNTIME_ROOT "" CACHE PATH "Pinned HEVC decoder runtime package")
set(COHAVORA_H265_RUNTIME_PROFILE
    "${PROJECT_SOURCE_DIR}/build/prepare/h265-runtime-profile.json" CACHE FILEPATH
    "Reviewed HEVC decoder runtime profile")
if(NOT COHAVORA_H265_RUNTIME_ROOT)
    return()
endif()
if(NOT COHAVORA_WEBRTC_PACKAGE_PROFILE)
    message(FATAL_ERROR "HEVC product decoding requires the reviewed codec SDK")
endif()
file(READ "${COHAVORA_H265_RUNTIME_PROFILE}" decoder_profile)
file(READ "${COHAVORA_H265_RUNTIME_ROOT}/metadata/package.json" decoder_package)
foreach(field schema_version package_id avcodec_major avutil_major)
    string(JSON expected GET "${decoder_profile}" ${field})
    string(JSON actual GET "${decoder_package}" ${field})
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "HEVC runtime package/profile ${field} mismatch")
    endif()
endforeach()
string(JSON count LENGTH "${decoder_profile}" files)
math(EXPR last "${count} - 1")
set(decoder_runtime_files)
foreach(index RANGE 0 ${last})
    string(JSON relative MEMBER "${decoder_profile}" files ${index})
    if(IS_ABSOLUTE "${relative}" OR relative MATCHES "(^|/)\\.\\.(/|$)|[\\\\:;]")
        message(FATAL_ERROR "Unsafe HEVC runtime path: ${relative}")
    endif()
    string(JSON expected GET "${decoder_profile}" files "${relative}")
    file(SHA256 "${COHAVORA_H265_RUNTIME_ROOT}/${relative}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "HEVC runtime fingerprint mismatch: ${relative}")
    endif()
    if(relative MATCHES "^bin/.*\\.dll$")
        list(APPEND decoder_runtime_files "${COHAVORA_H265_RUNTIME_ROOT}/${relative}")
    endif()
endforeach()
install(FILES ${decoder_runtime_files} DESTINATION media-codecs)
install(DIRECTORY "${COHAVORA_H265_RUNTIME_ROOT}/licenses/"
    DESTINATION media-codecs/licenses)
add_library(cohavora_h265_decoder OBJECT
    "${PROJECT_SOURCE_DIR}/src/rtc/h265_video_decoder.cpp")
target_compile_features(cohavora_h265_decoder PRIVATE cxx_std_20)
target_include_directories(cohavora_h265_decoder PRIVATE
    "${COHAVORA_H265_RUNTIME_ROOT}/include" "${PROJECT_SOURCE_DIR}/src"
    "${WEBRTC_ROOT}/include/third_party/abseil-cpp")
if(MSVC)
    target_compile_options(cohavora_h265_decoder PRIVATE /utf-8)
endif()
target_link_libraries(cohavora_h265_decoder PRIVATE cohavora_webrtc_sdk)
target_sources(cohavora_core PRIVATE $<TARGET_OBJECTS:cohavora_h265_decoder>)
target_compile_definitions(cohavora_core PUBLIC COHAVORA_HAS_H265_DECODER=1)

# The core is used by executables in the top-level configuration directory;
# the desktop client has its own output directory. No PATH changes are needed.
set_property(GLOBAL PROPERTY COHAVORA_H265_RUNTIME_FILES "${decoder_runtime_files}")
function(cohavora_create_h265_runtime_target)
    get_property(runtime_files GLOBAL PROPERTY COHAVORA_H265_RUNTIME_FILES)
    set(commands
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/$<CONFIG>/media-codecs"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${runtime_files} "${CMAKE_BINARY_DIR}/$<CONFIG>/media-codecs"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${COHAVORA_H265_RUNTIME_ROOT}/licenses" "${CMAKE_BINARY_DIR}/$<CONFIG>/media-codecs/licenses")
    if(TARGET cohavora_app)
        list(APPEND commands
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:cohavora_app>/media-codecs"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${runtime_files} "$<TARGET_FILE_DIR:cohavora_app>/media-codecs"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${COHAVORA_H265_RUNTIME_ROOT}/licenses" "$<TARGET_FILE_DIR:cohavora_app>/media-codecs/licenses")
    endif()
    add_custom_target(cohavora_h265_decoder_runtime ${commands} VERBATIM)
    add_dependencies(cohavora_core cohavora_h265_decoder_runtime)
endfunction()
cmake_language(DEFER CALL cohavora_create_h265_runtime_target)
message(STATUS "[HEVC] Product decoder runtime fingerprints verified")
