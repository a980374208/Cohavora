# The default SDK keeps its established integration. An explicitly selected
# reviewed profile binds separately built SDK archives and their static deps.
include_guard(GLOBAL)
# This is a configure-time capability, separate from the C++ definition.
# Reset it before the default-SDK early return on every configure.
set(COHAVORA_WEBRTC_HAS_BACKEND_ENCODERS OFF)
set(COHAVORA_WEBRTC_HAS_X265_ENCODER OFF)
set(COHAVORA_WEBRTC_HAS_LINUX_NVENC OFF)
set(COHAVORA_WEBRTC_HAS_LINUX_QSV OFF)
set(COHAVORA_WEBRTC_HAS_LINUX_VAAPI OFF)
set(COHAVORA_WEBRTC_LINUX_VAAPI_RUNTIME "")
if(NOT COHAVORA_WEBRTC_PACKAGE_PROFILE)
    if(COHAVORA_LINUX_HW_ENCODERS)
        message(FATAL_ERROR "Linux hardware encoders require a reviewed WebRTC package profile")
    endif()
    return()
endif()

file(READ "${COHAVORA_WEBRTC_PACKAGE_PROFILE}" cohavora_webrtc_profile)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${COHAVORA_WEBRTC_PACKAGE_PROFILE}")
string(JSON cohavora_webrtc_schema GET "${cohavora_webrtc_profile}" schema_version)
string(JSON cohavora_webrtc_package_id GET "${cohavora_webrtc_profile}" package_id)
string(JSON cohavora_webrtc_prefix GET "${cohavora_webrtc_profile}" boringssl_prefix)
if(NOT cohavora_webrtc_schema EQUAL 1 OR
   cohavora_webrtc_package_id STREQUAL "" OR
   NOT cohavora_webrtc_prefix STREQUAL "cohavora_bssl")
    message(FATAL_ERROR "Unsupported reviewed WebRTC package profile")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(JSON cohavora_webrtc_platform GET "${cohavora_webrtc_profile}" platform)
    if(NOT cohavora_webrtc_platform STREQUAL "linux-x86_64")
        message(FATAL_ERROR "Reviewed WebRTC package does not target Linux x86_64")
    endif()
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
if(cohavora_webrtc_definition_count LESS 1 OR cohavora_webrtc_definition_count GREATER 2)
    message(FATAL_ERROR "Reviewed codec SDK must declare RTC_ENABLE_H265")
endif()
set(cohavora_webrtc_public_definitions)
math(EXPR cohavora_webrtc_definition_last "${cohavora_webrtc_definition_count} - 1")
foreach(index RANGE 0 ${cohavora_webrtc_definition_last})
    string(JSON cohavora_webrtc_definition GET "${cohavora_webrtc_profile}"
        compile_definitions ${index})
    if(cohavora_webrtc_definition STREQUAL "RTC_ENABLE_H265")
        list(APPEND cohavora_webrtc_public_definitions "${cohavora_webrtc_definition}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32 AND
           cohavora_webrtc_definition STREQUAL "WEBRTC_LINUX_ENCODER_FACTORY_BINDING=1")
        list(APPEND cohavora_webrtc_public_definitions "${cohavora_webrtc_definition}")
    else()
        message(FATAL_ERROR "Unsupported reviewed WebRTC consumer compile definition")
    endif()
endforeach()
list(REMOVE_DUPLICATES cohavora_webrtc_public_definitions)
list(LENGTH cohavora_webrtc_public_definitions cohavora_webrtc_unique_definitions)
if(NOT cohavora_webrtc_unique_definitions EQUAL cohavora_webrtc_definition_count OR
   NOT "RTC_ENABLE_H265" IN_LIST cohavora_webrtc_public_definitions)
    message(FATAL_ERROR "Reviewed WebRTC consumer definitions are incomplete or duplicated")
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
    if(WIN32)
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
        set(cohavora_webrtc_archive "${WEBRTC_ROOT}/lib/${config}/webrtc.lib")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        foreach(field stdlib cxx11_abi glibcxx_debug rtti pic)
            string(JSON cohavora_webrtc_abi_${field} GET "${cohavora_webrtc_profile}"
                configurations "${config}" abi "${field}")
        endforeach()
        if(NOT cohavora_webrtc_abi_stdlib STREQUAL "libstdc++" OR
           NOT cohavora_webrtc_abi_cxx11_abi EQUAL 1 OR
           cohavora_webrtc_abi_glibcxx_debug OR
           NOT cohavora_webrtc_abi_rtti OR NOT cohavora_webrtc_abi_pic)
            message(FATAL_ERROR "Reviewed WebRTC ${config} Linux C++ ABI is incompatible")
        endif()
        set(cohavora_webrtc_archive "${WEBRTC_ROOT}/lib/${config}/libwebrtc.a")
    else()
        message(FATAL_ERROR "No reviewed WebRTC package integration for ${CMAKE_SYSTEM_NAME}")
    endif()
    cohavora_verify_webrtc_archive(
        "${cohavora_webrtc_archive}" "${expected_library}")

    string(JSON external_count LENGTH "${cohavora_webrtc_profile}"
        configurations "${config}" external_static_libraries)
    if(external_count LESS 1)
        message(FATAL_ERROR "Reviewed codec SDK ${config} has no external static libraries")
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT external_count EQUAL 1)
        message(FATAL_ERROR "Reviewed Linux codec SDK must contain only the x265 dependency")
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
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
           (NOT external_name STREQUAL "x265" OR
            NOT relative_path STREQUAL "lib/${config}/libx265.a"))
            message(FATAL_ERROR "Unsupported reviewed Linux codec dependency: ${external_name}")
        endif()
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

# Linux driver execution is an SDK contract, not a consumer definition inferred
# from the presence of a header. Older synchronous candidates remain usable with
# hardware disabled, but cannot enter the product hardware selection path.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND COHAVORA_LINUX_HW_ENCODERS)
    if(NOT "WEBRTC_LINUX_ENCODER_FACTORY_BINDING=1" IN_LIST cohavora_webrtc_public_definitions)
        message(FATAL_ERROR "Linux hardware SDK requires publication-owned encoder factory binding")
    endif()
    string(JSON binding_abi GET "${cohavora_webrtc_profile}"
        linux_publication_factory_binding abi)
    string(JSON binding_header_hash GET "${cohavora_webrtc_profile}"
        linux_publication_factory_binding public_sender_header_sha256)
    if(NOT binding_abi STREQUAL "rtp_sender_owned_factory_v1")
        message(FATAL_ERROR "Unsupported Linux publication factory binding ABI")
    endif()
    cohavora_verify_webrtc_archive(
        "${WEBRTC_ROOT}/include/api/rtp_sender_interface.h" "${binding_header_hash}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${WEBRTC_ROOT}/include/api/rtp_sender_interface.h")
    set(cohavora_linux_backend_count 0)
    foreach(backend NVENC QSV VAAPI)
        if(backend STREQUAL "NVENC")
            set(backend_key linux_nvenc_cuda)
            set(factory_name CreateLinuxNvencVideoEncoder)
            set(factory_header modules/video_coding/codecs/external/linux_nvenc_video_encoder.h)
        elseif(backend STREQUAL "QSV")
            set(backend_key linux_qsv_vaapi)
            set(factory_name CreateLinuxQsvVideoEncoder)
            set(factory_header modules/video_coding/codecs/external/linux_qsv_video_encoder.h)
        else()
            set(backend_key linux_vaapi)
            set(factory_name CreateLinuxVaapiVideoEncoder)
            set(factory_header modules/video_coding/codecs/external/linux_vaapi_video_encoder.h)
        endif()
        string(JSON backend_compiled ERROR_VARIABLE backend_missing
            GET "${cohavora_webrtc_profile}" compiled_backends "${backend_key}" compiled)
        if(backend_missing)
            continue()
        endif()
        string(JSON backend_compiled_type TYPE "${cohavora_webrtc_profile}"
            compiled_backends "${backend_key}" compiled)
        if(NOT backend_compiled_type STREQUAL "BOOLEAN")
            message(FATAL_ERROR "Linux encoder compiled flag must be boolean: ${backend_key}")
        endif()
        if(NOT backend_compiled)
            continue()
        endif()
        string(JSON backend_execution GET "${cohavora_webrtc_profile}"
            compiled_backends "${backend_key}" execution_model)
        string(JSON backend_factory GET "${cohavora_webrtc_profile}"
            compiled_backends "${backend_key}" explicit_factory)
        string(JSON backend_header_hash GET "${cohavora_webrtc_profile}"
            compiled_backends "${backend_key}" public_factory_header_sha256)
        if(NOT backend_execution STREQUAL "bounded_worker_v1" OR
           NOT backend_factory STREQUAL factory_name)
            message(FATAL_ERROR "Unsupported Linux encoder execution contract: ${backend_key}")
        endif()
        cohavora_verify_webrtc_archive(
            "${WEBRTC_ROOT}/include/${factory_header}" "${backend_header_hash}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${WEBRTC_ROOT}/include/${factory_header}")
        if(backend STREQUAL "VAAPI")
            string(JSON runtime_path GET "${cohavora_webrtc_profile}"
                compiled_backends "${backend_key}" runtime_relative_path)
            string(JSON runtime_hash GET "${cohavora_webrtc_profile}"
                compiled_backends "${backend_key}" runtime_sha256)
            string(JSON runtime_abi GET "${cohavora_webrtc_profile}"
                compiled_backends "${backend_key}" runtime_abi)
            if(NOT runtime_path STREQUAL
                    "runtime/linux-x86_64/libcohavora_vaapi_runtime.so.1" OR
               NOT runtime_abi EQUAL 2)
                message(FATAL_ERROR "Unsupported Linux VAAPI runtime contract")
            endif()
            set(COHAVORA_WEBRTC_LINUX_VAAPI_RUNTIME "${WEBRTC_ROOT}/${runtime_path}")
            file(REAL_PATH "${COHAVORA_WEBRTC_LINUX_VAAPI_RUNTIME}" runtime_real)
            string(FIND "${runtime_real}" "${cohavora_webrtc_real_root}/" runtime_root)
            if(NOT runtime_root EQUAL 0)
                message(FATAL_ERROR "Linux VAAPI runtime escapes the SDK root")
            endif()
            cohavora_verify_webrtc_archive(
                "${COHAVORA_WEBRTC_LINUX_VAAPI_RUNTIME}" "${runtime_hash}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${COHAVORA_WEBRTC_LINUX_VAAPI_RUNTIME}")
        endif()
        set(COHAVORA_WEBRTC_HAS_LINUX_${backend} ON)
        math(EXPR cohavora_linux_backend_count "${cohavora_linux_backend_count} + 1")
    endforeach()
    if(cohavora_linux_backend_count EQUAL 0)
        message(FATAL_ERROR "The selected SDK contains no reviewed Linux hardware encoder")
    endif()
endif()

# Enable extension sources only after all reviewed SDK/dependency archives
# have passed their fingerprint and ABI checks.
if(WIN32)
    set(COHAVORA_WEBRTC_HAS_BACKEND_ENCODERS ON)
else()
    set(COHAVORA_WEBRTC_HAS_X265_ENCODER ON)
endif()

if(COHAVORA_DEBUG_DEPENDENCY_ABI STREQUAL "Debug")
    set(cohavora_webrtc_debug_external_archives
        "${cohavora_webrtc_Debug_external_archives}")
else()
    set(cohavora_webrtc_debug_external_archives
        "${cohavora_webrtc_Release_external_archives}")
endif()
if(TARGET cohavora_webrtc_sdk)
    target_compile_definitions(cohavora_webrtc_sdk INTERFACE ${cohavora_webrtc_public_definitions})
    if(TARGET cohavora_core)
        # Core exposes WebRTC headers publicly but links the SDK privately.
        # Carry only this header ABI requirement to application/test users.
        target_compile_definitions(cohavora_core PUBLIC ${cohavora_webrtc_public_definitions})
        if(WIN32)
            target_compile_definitions(cohavora_core PUBLIC COHAVORA_HAS_BACKEND_ENCODERS=1)
        else()
            target_compile_definitions(cohavora_core PUBLIC COHAVORA_HAS_X265_ENCODER=1)
            if(COHAVORA_LINUX_HW_ENCODERS)
                target_compile_definitions(cohavora_core PUBLIC COHAVORA_HAS_LINUX_HW_ENCODERS=1)
                foreach(backend NVENC QSV VAAPI)
                    if(COHAVORA_WEBRTC_HAS_LINUX_${backend})
                        target_compile_definitions(cohavora_core PUBLIC COHAVORA_HAS_LINUX_${backend}=1)
                    endif()
                endforeach()
            endif()
        endif()
    endif()
    if(WIN32)
        foreach(archive IN LISTS cohavora_webrtc_debug_external_archives)
            target_link_libraries(cohavora_webrtc_sdk INTERFACE
                "$<$<CONFIG:Debug>:${archive}>")
        endforeach()
        foreach(archive IN LISTS cohavora_webrtc_Release_external_archives)
            target_link_libraries(cohavora_webrtc_sdk INTERFACE
                "$<$<NOT:$<CONFIG:Debug>>:${archive}>")
        endforeach()
    endif()
endif()
message(STATUS "[WebRTC] Reviewed codec SDK archives verified: ${cohavora_webrtc_package_id}")
