cmake_minimum_required(VERSION 3.20)
project(livekit_desktop_gl_validation LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
get_filename_component(LIVEKIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets)
if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

# Rebuild the exact production module and host against a desktop Qt kit.
# No alternate renderer implementation and no modification of the bundled Qt.
add_library(cohavora_render_opengl MODULE "${LIVEKIT_ROOT}/src/render/modules/opengl_backend.cpp")
set_target_properties(cohavora_render_opengl PROPERTIES AUTOMOC OFF
    OUTPUT_NAME "cohavora-render-opengl" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
target_compile_definitions(cohavora_render_opengl PRIVATE LK_RENDER_MODULE_BUILD)
target_include_directories(cohavora_render_opengl PRIVATE "${LIVEKIT_ROOT}" "${LIVEKIT_ROOT}/src")
add_executable(desktop_gl_probe
    desktop_gl_probe.cpp
    "${LIVEKIT_ROOT}/tests/render_p3/opengl_contract.cpp"
    "${LIVEKIT_ROOT}/src/rtc/video_frame.cpp"
    "${LIVEKIT_ROOT}/src/render/video_render_frame.cpp"
    "${LIVEKIT_ROOT}/src/render/backend_module.cpp"
    "${LIVEKIT_ROOT}/src/ui/render/video_canvas.cpp"
    "${LIVEKIT_ROOT}/src/ui/render/module_video_canvas.cpp"
    "${LIVEKIT_ROOT}/src/ui/render/gl_video_canvas.cpp"
    "${LIVEKIT_ROOT}/src/ui/render/video_canvas_factory.cpp")
target_compile_definitions(desktop_gl_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(desktop_gl_probe PRIVATE "${LIVEKIT_ROOT}" "${LIVEKIT_ROOT}/src")
target_link_libraries(desktop_gl_probe PRIVATE Qt5::Widgets Qt5::Gui Qt5::Core d3d11 ${CMAKE_DL_LIBS})
set(_desktop_runtime_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
set_target_properties(desktop_gl_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${_desktop_runtime_dir}")
add_executable(driver_reset_control driver_reset_control.cpp)
set_target_properties(driver_reset_control PROPERTIES AUTOMOC OFF)
target_link_libraries(driver_reset_control PRIVATE gdi32 user32)

get_target_property(_qmake Qt5::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin "${_qmake}" DIRECTORY)

function(livekit_imported_runtime_name output target config)
    get_target_property(_location ${target} IMPORTED_LOCATION_${config})
    if(NOT _location OR _location MATCHES "-NOTFOUND$")
        get_target_property(_mapped_configs ${target} MAP_IMPORTED_CONFIG_${config})
        foreach(_mapped_config IN LISTS _mapped_configs)
            string(TOUPPER "${_mapped_config}" _mapped_config)
            get_target_property(_location ${target}
                IMPORTED_LOCATION_${_mapped_config})
            if(_location AND NOT _location MATCHES "-NOTFOUND$")
                break()
            endif()
        endforeach()
    endif()
    if(NOT _location OR _location MATCHES "-NOTFOUND$")
        foreach(_fallback_config IN ITEMS RELEASE RELWITHDEBINFO MINSIZEREL DEBUG)
            get_target_property(_location ${target}
                IMPORTED_LOCATION_${_fallback_config})
            if(_location AND NOT _location MATCHES "-NOTFOUND$")
                break()
            endif()
        endforeach()
    endif()
    if(NOT _location OR _location MATCHES "-NOTFOUND$")
        get_target_property(_location ${target} IMPORTED_LOCATION)
    endif()
    if(NOT _location OR _location MATCHES "-NOTFOUND$")
        message(FATAL_ERROR
            "Unable to resolve the ${config} runtime for ${target}")
    endif()
    get_filename_component(_name "${_location}" NAME)
    set(${output} "${_name}" PARENT_SCOPE)
endfunction()

set(_qt_runtime_outputs)
set(_qt_runtime_copy_commands)
foreach(_component IN ITEMS Core Gui Widgets)
    livekit_imported_runtime_name(_debug_name Qt5::${_component} DEBUG)
    livekit_imported_runtime_name(_release_name Qt5::${_component} RELEASE)
    if(_debug_name STREQUAL _release_name)
        set(_runtime_name "${_release_name}")
    else()
        set(_runtime_name
            "$<IF:$<CONFIG:Debug>,${_debug_name},${_release_name}>")
    endif()
    set(_runtime_output "${_desktop_runtime_dir}/${_runtime_name}")
    list(APPEND _qt_runtime_outputs "${_runtime_output}")
    list(APPEND _qt_runtime_copy_commands
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt5::${_component}>" "${_runtime_output}")
endforeach()

set(_renderer_runtime_output
    "${_desktop_runtime_dir}/renderers/${CMAKE_SHARED_MODULE_PREFIX}cohavora-render-opengl${CMAKE_SHARED_MODULE_SUFFIX}")
set(_platform_runtime_source
    "${_qt_bin}/../plugins/platforms/qwindows$<$<CONFIG:Debug>:d>.dll")
set(_platform_runtime_output
    "${_desktop_runtime_dir}/platforms/qwindows$<$<CONFIG:Debug>:d>.dll")

# This relocated kit's qmake reports another installed Qt's prefix. Model each
# deployed runtime as an output so an incremental build restores deleted files
# and reruns only when a source binary changes.
add_custom_command(
    OUTPUT
        "${_renderer_runtime_output}"
        ${_qt_runtime_outputs}
        "${_platform_runtime_output}"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${_desktop_runtime_dir}/renderers"
        "${_desktop_runtime_dir}/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:cohavora_render_opengl>"
        "${_renderer_runtime_output}"
    ${_qt_runtime_copy_commands}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_platform_runtime_source}"
        "${_platform_runtime_output}"
    DEPENDS
        "$<TARGET_FILE:cohavora_render_opengl>"
        "$<TARGET_FILE:Qt5::Core>"
        "$<TARGET_FILE:Qt5::Gui>"
        "$<TARGET_FILE:Qt5::Widgets>"
        "${_platform_runtime_source}"
    COMMENT "Deploying the desktop OpenGL validation runtime"
    VERBATIM)
add_custom_target(deploy_desktop_gl_runtime
    DEPENDS
        "${_renderer_runtime_output}"
        ${_qt_runtime_outputs}
        "${_platform_runtime_output}")
add_dependencies(desktop_gl_probe deploy_desktop_gl_runtime)
enable_testing()
add_test(NAME desktop_gl_contract COMMAND desktop_gl_probe --contract)
add_test(NAME desktop_gl_surface COMMAND desktop_gl_probe --output "${CMAKE_BINARY_DIR}/recovery/evidence/dpi100")
add_test(NAME desktop_gl_surface_hidpi COMMAND desktop_gl_probe --output "${CMAKE_BINARY_DIR}/recovery/evidence/dpi150")
add_test(NAME desktop_gl_blocked_present COMMAND desktop_gl_probe --block-present --output "${CMAKE_BINARY_DIR}/recovery/evidence/blocked")
set_tests_properties(desktop_gl_contract desktop_gl_surface desktop_gl_surface_hidpi desktop_gl_blocked_present
    PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
set_tests_properties(desktop_gl_surface PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=1")
set_tests_properties(desktop_gl_surface_hidpi PROPERTIES ENVIRONMENT "QT_SCALE_FACTOR=1.5")
# Deliberate system driver resets are never part of unattended CTest. --wait-driver-loss
# only observes the driver; an external, explicitly authorized reset triggers the loss.
