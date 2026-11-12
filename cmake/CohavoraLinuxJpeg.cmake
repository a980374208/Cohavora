include_guard(GLOBAL)

# Chromium mangles JPEG functions, but its SDK leaves two data tables public.
# Rewrite definitions AND references in a build-local archive, keeping both
# incompatible JPEG ABIs and the original SDK intact.
function(cohavora_isolate_linux_jpeg)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(NOT CMAKE_NM OR NOT CMAKE_OBJCOPY)
        message(FATAL_ERROR "Linux JPEG isolation requires nm and objcopy")
    endif()
    set(qt_jpeg_archives
        "${QT_ROOT}/lib/libqtlibjpeg.a"
        "${QT_ROOT}/lib/libqtlibjpeg12bits.a"
        "${QT_ROOT}/lib/libqtlibjpeg16bits.a")
    set(qt_jpeg_args)
    foreach(archive IN LISTS qt_jpeg_archives)
        if(NOT EXISTS "${archive}")
            message(FATAL_ERROR "Missing matching Qt JPEG archive: ${archive}")
        endif()
        list(APPEND qt_jpeg_args --qt-jpeg "${archive}")
    endforeach()
    set(configs Release)
    if(COHAVORA_DEBUG_DEPENDENCY_ABI STREQUAL "Debug")
        list(APPEND configs Debug)
    endif()
    foreach(config IN LISTS configs)
        string(TOUPPER "${config}" upper_config)
        set(source "${WEBRTC_${upper_config}_LIB}")
        set(output "${CMAKE_BINARY_DIR}/dependencies/webrtc-jpeg/${config}/libwebrtc.a")
        set(isolated_${config} "${output}")
        add_custom_command(OUTPUT "${output}" "${CMAKE_BINARY_DIR}/dependencies/webrtc-jpeg/${config}/libwebrtc.json"
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/build/isolate_linux_jpeg.py"
                --archive "${source}" --output "${output}"
                --nm "${CMAKE_NM}" --objcopy "${CMAKE_OBJCOPY}" ${qt_jpeg_args}
            DEPENDS "${source}" ${qt_jpeg_archives}
                "${PROJECT_SOURCE_DIR}/build/isolate_linux_jpeg.py"
            COMMENT "Isolating WebRTC JPEG data symbols (${config}); preserving SDK"
            VERBATIM)
    endforeach()
    if(NOT DEFINED isolated_Debug)
        set(isolated_Debug "${isolated_Release}")
    endif()
    add_custom_target(cohavora_webrtc_jpeg_isolation DEPENDS
        "$<IF:$<CONFIG:Debug>,${isolated_Debug},${isolated_Release}>")
    add_dependencies(cohavora_webrtc_sdk cohavora_webrtc_jpeg_isolation)
    get_target_property(link_libraries cohavora_webrtc_sdk INTERFACE_LINK_LIBRARIES)
    string(REPLACE "${WEBRTC_RELEASE_LIB}" "${isolated_Release}" link_libraries "${link_libraries}")
    string(REPLACE "${WEBRTC_DEBUG_LIB}" "${isolated_Debug}" link_libraries "${link_libraries}")
    set_property(TARGET cohavora_webrtc_sdk PROPERTY INTERFACE_LINK_LIBRARIES "${link_libraries}")
endfunction()
