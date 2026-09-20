cmake_minimum_required(VERSION 3.20)
project(livekit_desktop_gl_validation LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
get_filename_component(LIVEKIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets)
if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

# Rebuild the exact production module and host against a desktop Qt kit.
# No alternate renderer implementation and no modification of the bundled Qt.
add_library(livekit_render_opengl MODULE "${LIVEKIT_ROOT}/src/render/modules/opengl_backend.cpp")
set_target_properties(livekit_render_opengl PROPERTIES AUTOMOC OFF
    OUTPUT_NAME "livekit-render-opengl" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
target_compile_definitions(livekit_render_opengl PRIVATE LK_RENDER_MODULE_BUILD)
target_include_directories(livekit_render_opengl PRIVATE "${LIVEKIT_ROOT}" "${LIVEKIT_ROOT}/src")
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
add_executable(driver_reset_control driver_reset_control.cpp)
set_target_properties(driver_reset_control PROPERTIES AUTOMOC OFF)
target_link_libraries(driver_reset_control PRIVATE gdi32 user32)
add_custom_target(deploy_desktop_gl
    DEPENDS livekit_render_opengl
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:desktop_gl_probe>/renderers"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:livekit_render_opengl>"
        "$<TARGET_FILE_DIR:desktop_gl_probe>/renderers/$<TARGET_FILE_NAME:livekit_render_opengl>")
add_dependencies(desktop_gl_probe deploy_desktop_gl)

get_target_property(_qmake Qt5::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
add_custom_command(TARGET desktop_gl_probe POST_BUILD
    # This relocated kit's qmake reports another installed Qt's prefix. Copy
    # imported targets explicitly instead of deploying libraries from that kit.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Qt5::Core>"
        "$<TARGET_FILE:Qt5::Gui>" "$<TARGET_FILE:Qt5::Widgets>" "$<TARGET_FILE_DIR:desktop_gl_probe>"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:desktop_gl_probe>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_qt_bin}/../plugins/platforms/qwindows$<$<CONFIG:Debug>:d>.dll"
        "$<TARGET_FILE_DIR:desktop_gl_probe>/platforms")
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
