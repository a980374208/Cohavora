# The default SDK keeps its established integration. An explicitly selected
# reviewed profile binds separately built SDK archives and their static deps.
include_guard(GLOBAL)
# This is a configure-time capability, separate from the C++ definition.
# Reset it before the default-SDK early return on every configure.
set(COHAVORA_WEBRTC_HAS_BACKEND_ENCODERS OFF)
if(NOT COHAVORA_WEBRTC_PACKAGE_PROFILE)
    return()
endif()

file(READ "${COHAVORA_WEBRTC_PACKAGE_PROFILE}" cohavora_webrtc_profile)
string(JSON cohavora_webrtc_schema GET "${cohavora_webrtc_profile}" schema_version)
string(JSON cohavora_webrtc_package_id GET "${cohavora_webrtc_profile}" package_id)
string(JSON cohavora_webrtc_prefix GET "${cohavora_webrtc_profile}" boringssl_prefix)
if(NOT cohavora_webrtc_schema EQUAL 1 OR
   cohavora_webrtc_package_id STREQUAL "" OR
   NOT cohavora_webrtc_prefix STREQUAL "cohavora_bssl")
    message(FATAL_ERROR "Unsupported reviewed WebRTC package profile")
endif()

# This public macro selects SDK header layouts and enables the paired E2EE
# overlay's HEVC handling. Keep BoringSSL and private vendor switches out of
# application compilation; only reviewed consumer definitions are accepted.
string(JSON cohavora_webrtc_definition_type TYPE "${cohavora_webrtc_profile}"
    compile_definitions)
if(NOT cohavora_webrtc_definition_type STREQUAL "ARRAY")
    message(FATAL_ERROR "Reviewed WebRTC compile_definitions must be an array")
endif()
string(JSON cohavora_webrtc_definition_count LENGTH "${cohavora_webrtc_profile}"
    compile_definitions)
if(NOT cohavora_webrtc_definition_count EQUAL 1)
    message(FATAL_ERROR "Reviewed codec SDK must declare RTC_ENABLE_H265")
endif()
string(JSON cohavora_webrtc_definition GET "${cohavora_webrtc_profile}"
    compile_definitions 0)
if(NOT cohavora_webrtc_definition STREQUAL "RTC_ENABLE_H265")
    message(FATAL_ERROR "Unsupported reviewed WebRTC consumer compile definition")
endif()

function(cohavora_verify_webrtc_archive archive expected_hash)
    string(LENGTH "${expected_hash}" hash_length)
    if(NOT hash_length EQUAL 64 OR NOT expected_hash MATCHES "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR "Invalid reviewed WebRTC archive SHA256: ${archive}")
    endif()
    if(NOT EXISTS "${archive}" OR IS_DIRECTORY "${archive}")
        message(FATAL_ERROR "Reviewed WebRTC archive is missing: ${archive}")
    endif()
    file(SHA256 "${archive}" actual_hash)
    string(TOLOWER "${expected_hash}" expected_hash)
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR "Reviewed WebRTC archive fingerprint mismatch: ${archive}")
    endif()
endfunction()

file(REAL_PATH "${WEBRTC_ROOT}" cohavora_webrtc_real_root)
foreach(config Debug Release)
    string(JSON expected_library GET "${cohavora_webrtc_profile}"
        configurations "${config}" library_sha256)
    string(JSON runtime_library GET "${cohavora_webrtc_profile}"
        configurations "${config}" runtime_library)
    string(JSON iterator_debug_level GET "${cohavora_webrtc_profile}"
        configurations "${config}" iterator_debug_level)
    if(config STREQUAL "Debug")
        set(expected_runtime "MTd_StaticDebug")
        set(expected_iterator 2)
    else()
        set(expected_runtime "MT_StaticRelease")
        set(expected_iterator 0)
    endif()
    if(NOT runtime_library STREQUAL expected_runtime OR
       NOT iterator_debug_level EQUAL expected_iterator)
        message(FATAL_ERROR "Reviewed WebRTC ${config} CRT/STL profile is incompatible")
    endif()
    cohavora_verify_webrtc_archive(
        "${WEBRTC_ROOT}/lib/${config}/webrtc.lib" "${expected_library}")

    string(JSON external_count LENGTH "${cohavora_webrtc_profile}"
        configurations "${config}" external_static_libraries)
    if(external_count LESS 1)
        message(FATAL_ERROR "Reviewed codec SDK ${config} has no external static libraries")
    endif()
    math(EXPR external_last "${external_count} - 1")
    set(external_archives)
    foreach(index RANGE 0 ${external_last})
        string(JSON external_name MEMBER "${cohavora_webrtc_profile}"
            configurations "${config}" external_static_libraries ${index})
        string(JSON relative_path GET "${cohavora_webrtc_profile}"
            configurations "${config}" external_static_libraries "${external_name}" relative_path)
        string(JSON external_hash GET "${cohavora_webrtc_profile}"
            configurations "${config}" external_static_libraries "${external_name}" sha256)
        string(FIND "${relative_path}" "\\" backslash_index)
        string(FIND "${relative_path}" ":" colon_index)
        string(FIND "${relative_path}" ";" semicolon_index)
        if(NOT external_name MATCHES "^[A-Za-z0-9_]+$" OR
           IS_ABSOLUTE "${relative_path}" OR
           relative_path STREQUAL "" OR
           relative_path MATCHES "(^|/)\\.\\.(/|$)" OR
           NOT backslash_index EQUAL -1 OR
           NOT colon_index EQUAL -1 OR
           NOT semicolon_index EQUAL -1)
            message(FATAL_ERROR "Unsafe reviewed WebRTC external archive path: ${relative_path}")
        endif()
        set(archive "${WEBRTC_ROOT}/${relative_path}")
        file(REAL_PATH "${archive}" archive_real)
        string(FIND "${archive_real}" "${cohavora_webrtc_real_root}/" root_index)
        if(NOT root_index EQUAL 0)
            message(FATAL_ERROR "Reviewed WebRTC external archive escapes SDK root: ${relative_path}")
        endif()
        cohavora_verify_webrtc_archive("${archive}" "${external_hash}")
        list(APPEND external_archives "${archive}")
    endforeach()
    set(cohavora_webrtc_${config}_external_archives "${external_archives}")
endforeach()

# Enable extension sources only after all reviewed SDK/dependency archives
# have passed their fingerprint and ABI checks.
set(COHAVORA_WEBRTC_HAS_BACKEND_ENCODERS ON)

if(COHAVORA_DEBUG_DEPENDENCY_ABI STREQUAL "Debug")
    set(cohavora_webrtc_debug_external_archives
        "${cohavora_webrtc_Debug_external_archives}")
else()
    set(cohavora_webrtc_debug_external_archives
        "${cohavora_webrtc_Release_external_archives}")
endif()
if(TARGET cohavora_webrtc_sdk)
    target_compile_definitions(cohavora_webrtc_sdk INTERFACE RTC_ENABLE_H265)
    if(TARGET cohavora_core)
        # Core exposes WebRTC headers publicly but links the SDK privately.
        # Carry only this header ABI requirement to application/test users.
        target_compile_definitions(cohavora_core PUBLIC RTC_ENABLE_H265)
        target_compile_definitions(cohavora_core PUBLIC COHAVORA_HAS_BACKEND_ENCODERS=1)
    endif()
    foreach(archive IN LISTS cohavora_webrtc_debug_external_archives)
        target_link_libraries(cohavora_webrtc_sdk INTERFACE
            "$<$<CONFIG:Debug>:${archive}>")
    endforeach()
    foreach(archive IN LISTS cohavora_webrtc_Release_external_archives)
        target_link_libraries(cohavora_webrtc_sdk INTERFACE
            "$<$<NOT:$<CONFIG:Debug>>:${archive}>")
    endforeach()
endif()
message(STATUS "[WebRTC] Reviewed codec SDK archives verified: ${cohavora_webrtc_package_id}")
