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
    foreach(backend dx11 opengl)
        if(TARGET cohavora_render_${backend})
            add_custom_target(${target}_renderer_${backend}
                DEPENDS cohavora_render_${backend}
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/renderers"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:cohavora_render_${backend}>"
                    "$<TARGET_FILE_DIR:${target}>/renderers/$<TARGET_FILE_NAME:cohavora_render_${backend}>")
            add_dependencies(${target} ${target}_renderer_${backend})
        endif()
    endforeach()
endfunction()
