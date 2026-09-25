add_library(cohavora_qt_video_render STATIC
    qt_cpu_video_renderer.h
    qt_cpu_video_renderer.cpp
    video_render_session.h
    video_render_session.cpp)
target_include_directories(cohavora_qt_video_render PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${WEBRTC_ROOT}/include/third_party/libyuv/include)
target_link_libraries(cohavora_qt_video_render PUBLIC
    cohavora_core
    cohavora::qt_gui_runtime)
set_target_properties(cohavora_qt_video_render PROPERTIES
    AUTOMOC OFF
    FOLDER "render")
if(MSVC)
    target_compile_options(cohavora_qt_video_render PRIVATE /utf-8)
endif()

# Render backends are deployed beside consumers and never linked into the UI.
# No Qt/WebRTC runtime or C++ exports cross the module boundary.
if(WIN32)
    add_library(cohavora_render_dx11 MODULE
        modules/dx11_backend.cpp
        ${PROJECT_SOURCE_DIR}/src/ui/dx11/dx11_renderer.cpp)
    set_target_properties(cohavora_render_dx11 PROPERTIES AUTOMOC OFF
        PREFIX "" OUTPUT_NAME "cohavora-render-dx11" CXX_VISIBILITY_PRESET hidden)
    target_include_directories(cohavora_render_dx11 PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(cohavora_render_dx11 PRIVATE LK_RENDER_MODULE_BUILD)
    target_link_libraries(cohavora_render_dx11 PRIVATE d3d11 dxgi d3dcompiler)
    if(MSVC)
        target_compile_options(cohavora_render_dx11 PRIVATE /utf-8)
    endif()
endif()

add_library(cohavora_render_opengl MODULE modules/opengl_backend.cpp)
set_target_properties(cohavora_render_opengl PROPERTIES AUTOMOC OFF
    OUTPUT_NAME "cohavora-render-opengl" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(APPLE)
    set_target_properties(cohavora_render_opengl PROPERTIES SUFFIX ".dylib")
endif()
target_include_directories(cohavora_render_opengl PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(cohavora_render_opengl PRIVATE LK_RENDER_MODULE_BUILD)
if(MSVC)
    target_compile_options(cohavora_render_opengl PRIVATE /utf-8)
endif()

function(livekit_deploy_renderer target)
    cmake_parse_arguments(PARSE_ARGV 1 renderer "" "GROUP;DESTINATION" "")
    if(NOT renderer_GROUP OR NOT renderer_DESTINATION)
        message(FATAL_ERROR
            "livekit_deploy_renderer(${target}) requires GROUP and DESTINATION")
    endif()

    string(MAKE_C_IDENTIFIER "${renderer_GROUP}" renderer_group_id)
    foreach(backend dx11 opengl)
        if(TARGET cohavora_render_${backend})
            set(renderer_backend_target cohavora_render_${backend})
            set(renderer_deploy_target
                cohavora_deploy_renderer_${renderer_group_id}_${backend})

            if(NOT TARGET ${renderer_deploy_target})
                get_target_property(renderer_output_name
                    ${renderer_backend_target} OUTPUT_NAME)
                if(renderer_output_name MATCHES "-NOTFOUND$")
                    set(renderer_output_name "${renderer_backend_target}")
                endif()

                get_target_property(renderer_prefix
                    ${renderer_backend_target} PREFIX)
                if(renderer_prefix MATCHES "-NOTFOUND$")
                    set(renderer_prefix "${CMAKE_SHARED_MODULE_PREFIX}")
                endif()

                get_target_property(renderer_suffix
                    ${renderer_backend_target} SUFFIX)
                if(renderer_suffix MATCHES "-NOTFOUND$")
                    set(renderer_suffix "${CMAKE_SHARED_MODULE_SUFFIX}")
                endif()

                set(renderer_destination_dir
                    "${renderer_DESTINATION}/renderers")
                set(renderer_output
                    "${renderer_destination_dir}/${renderer_prefix}${renderer_output_name}${renderer_suffix}")
                add_custom_command(
                    OUTPUT "${renderer_output}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${renderer_destination_dir}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:${renderer_backend_target}>"
                        "${renderer_output}"
                    DEPENDS ${renderer_backend_target}
                    COMMENT "Deploying ${backend} renderer for ${renderer_GROUP}"
                    VERBATIM)
                add_custom_target(${renderer_deploy_target}
                    DEPENDS "${renderer_output}")
                set_target_properties(${renderer_deploy_target} PROPERTIES
                    FOLDER "render/deploy")
            endif()

            add_dependencies(${target} ${renderer_deploy_target})
        endif()
    endforeach()
endfunction()
