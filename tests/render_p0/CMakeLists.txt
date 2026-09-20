cmake_minimum_required(VERSION 3.20)
project(livekit_render_p0 LANGUAGES C CXX)

# Intentionally standalone: does not configure WebRTC, tdesktop or the meeting
# application. P0 is a design milestone, not a severity-P0 product modification.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(LIVEKIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
option(LIVEKIT_P0_BUILD_SURFACE "Build the Qt graphics probe (requires a desktop)" ON)
set(_bundled_default OFF)
if(WIN32 AND EXISTS "${LIVEKIT_ROOT}/deps/Libraries/win64/Qt-5.15.18")
    set(_bundled_default ON)
endif()
option(LIVEKIT_P0_BUNDLED_QT "Use this project's static Windows Qt/ANGLE package" ${_bundled_default})
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
    add_compile_options(/W4 /utf-8)
endif()

enable_testing()
add_library(render_abi_probe MODULE abi_probe.c)
target_compile_definitions(render_abi_probe PRIVATE LK_RENDER_MODULE_BUILD)
target_include_directories(render_abi_probe PRIVATE "${LIVEKIT_ROOT}/src")
set_target_properties(render_abi_probe PROPERTIES C_VISIBILITY_PRESET hidden)
add_executable(test_render_abi abi_contract.cpp)
target_include_directories(test_render_abi PRIVATE "${LIVEKIT_ROOT}/src")
target_link_libraries(test_render_abi PRIVATE ${CMAKE_DL_LIBS})
add_dependencies(test_render_abi render_abi_probe)
add_test(NAME render_abi_contract COMMAND test_render_abi $<TARGET_FILE:render_abi_probe>)

if(LIVEKIT_P0_BUILD_SURFACE)
    add_executable(render_surface_probe surface_probe.cpp)
    if(LIVEKIT_P0_BUNDLED_QT)
        # Match the existing app's relocated, static Qt package. Its .prl files
        # contain build-machine paths; do not edit vendor files to repair them.
        set(_libs "${LIVEKIT_ROOT}/deps/Libraries/win64")
        set(_qt "${_libs}/Qt-5.15.18")
        target_compile_definitions(render_surface_probe PRIVATE
            QT_STATIC QT_CORE_LIB QT_GUI_LIB QT_WIDGETS_LIB NOMINMAX LIVEKIT_P0_STATIC_WINDOWS_QT)
        target_include_directories(render_surface_probe SYSTEM PRIVATE
            "${_qt}/include" "${_qt}/include/QtCore" "${_qt}/include/QtGui" "${_qt}/include/QtWidgets"
            "${_libs}/tg_angle/include")
        foreach(_lib Qt5Widgets Qt5Gui Qt5Core Qt5FontDatabaseSupport Qt5EventDispatcherSupport
                     Qt5ThemeSupport Qt5WindowsUIAutomationSupport Qt5AccessibilitySupport
                     Qt5DeviceDiscoverySupport Qt5EdidSupport qtfreetype qtharfbuzz qtlibpng qtpcre2)
            target_link_libraries(render_surface_probe PRIVATE "${_qt}/lib/${_lib}.lib")
        endforeach()
        target_link_libraries(render_surface_probe PRIVATE
            "${_qt}/plugins/platforms/qwindows.lib"
            "${_libs}/tg_angle/out/Release/tg_angle.lib"
            "${_libs}/zlib/Release/zlibstatic.lib"
            d3d9 d3d11 dxgi dxguid d3dcompiler dwmapi dwrite d2d1
            winspool shlwapi wtsapi32 netapi32 userenv imm32 winmm version mpr ws2_32
            comdlg32 oleaut32 uxtheme)
    else()
        find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets)
        target_link_libraries(render_surface_probe PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
    endif()
    add_test(NAME render_surface_runtime COMMAND render_surface_probe --self-test
        --output "${CMAKE_CURRENT_BINARY_DIR}/evidence")
    set_tests_properties(render_surface_runtime PROPERTIES LABELS "RENDER_P0_RUNTIME"
        TIMEOUT 45 RUN_SERIAL TRUE)
endif()
