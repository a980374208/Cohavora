include_guard(GLOBAL)

set(COHAVORA_QT_VERSION "5.15.18")

function(cohavora_import_release_archive target location)
    if(TARGET ${target})
        return()
    endif()
    if(NOT EXISTS "${location}")
        message(FATAL_ERROR
            "The prebuilt dependency for ${target} is missing: ${location}")
    endif()

    add_library(${target} STATIC IMPORTED GLOBAL)
    set_target_properties(${target} PROPERTIES
        IMPORTED_CONFIGURATIONS Release
        IMPORTED_LOCATION "${location}"
        IMPORTED_LOCATION_RELEASE "${location}"
        MAP_IMPORTED_CONFIG_DEBUG Release
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        MAP_IMPORTED_CONFIG_MINSIZEREL Release)
endfunction()

function(cohavora_copy_qt_compile_usage target qt_target)
    foreach(property IN ITEMS
            INTERFACE_COMPILE_DEFINITIONS
            INTERFACE_COMPILE_FEATURES
            INTERFACE_COMPILE_OPTIONS
            INTERFACE_INCLUDE_DIRECTORIES)
        get_target_property(value ${qt_target} ${property})
        if(value AND NOT value MATCHES "-NOTFOUND$")
            set_property(TARGET ${target} APPEND PROPERTY ${property} "${value}")
        endif()
    endforeach()
endfunction()

function(cohavora_map_qt_target_to_release target)
    set_property(TARGET ${target} PROPERTY MAP_IMPORTED_CONFIG_DEBUG Release)
    set_property(TARGET ${target} PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
    set_property(TARGET ${target} PROPERTY MAP_IMPORTED_CONFIG_MINSIZEREL Release)

    foreach(property IN ITEMS LOCATION IMPLIB SONAME)
        get_target_property(release_value ${target} IMPORTED_${property}_RELEASE)
        if(release_value AND NOT release_value MATCHES "-NOTFOUND$")
            set_property(TARGET ${target} PROPERTY
                IMPORTED_${property}_DEBUG "${release_value}")
        endif()
    endforeach()

    # Qt 5's static package embeds explicit CONFIG:Debug expressions in its
    # interfaces. The project deliberately consumes the Release ABI in every
    # configuration, so use the package's own Release dependency closure.
    get_target_property(release_dependencies ${target}
        IMPORTED_LINK_INTERFACE_LIBRARIES_RELEASE)
    if(release_dependencies AND NOT release_dependencies MATCHES "-NOTFOUND$")
        set_property(TARGET ${target} PROPERTY
            INTERFACE_LINK_LIBRARIES "${release_dependencies}")
    endif()
endfunction()

macro(cohavora_configure_qt_dependencies)
    cmake_parse_arguments(qt "" "QT_ROOT;LIBRARIES_ROOT" "" ${ARGN})
    if(NOT qt_QT_ROOT OR NOT qt_LIBRARIES_ROOT)
        message(FATAL_ERROR
            "cohavora_configure_qt_dependencies requires QT_ROOT and LIBRARIES_ROOT")
    endif()
    if(TARGET cohavora_qt_application_runtime)
        message(FATAL_ERROR "Cohavora Qt dependencies were configured more than once")
    endif()

    set(qt_package_dir "${qt_QT_ROOT}/lib/cmake/Qt5")
    if(NOT EXISTS "${qt_package_dir}/Qt5Config.cmake")
        message(FATAL_ERROR
            "Qt ${COHAVORA_QT_VERSION} package metadata is missing: ${qt_package_dir}")
    endif()

    get_property(imported_before DIRECTORY PROPERTY IMPORTED_TARGETS)
    find_package(Qt5 ${COHAVORA_QT_VERSION} EXACT CONFIG REQUIRED
        COMPONENTS Core Gui Widgets Svg Network
        PATHS "${qt_package_dir}"
        NO_DEFAULT_PATH)
    if(NOT Qt5Core_VERSION VERSION_EQUAL COHAVORA_QT_VERSION)
        message(FATAL_ERROR
            "Expected Qt ${COHAVORA_QT_VERSION}, found ${Qt5Core_VERSION} at ${Qt5_DIR}")
    endif()
    get_property(imported_after DIRECTORY PROPERTY IMPORTED_TARGETS)
    list(REMOVE_ITEM imported_after ${imported_before})
    foreach(imported_target IN LISTS imported_after)
        if(imported_target MATCHES "^Qt5::")
            cohavora_map_qt_target_to_release(${imported_target})
        endif()
    endforeach()

    foreach(tool IN ITEMS rcc moc uic qmake)
        if(TARGET Qt5::${tool})
            set(tool_path "${qt_QT_ROOT}/bin/${tool}.exe")
            set_target_properties(Qt5::${tool} PROPERTIES
                IMPORTED_LOCATION "${tool_path}"
                IMPORTED_LOCATION_DEBUG "${tool_path}"
                IMPORTED_LOCATION_RELEASE "${tool_path}")
        endif()
    endforeach()

    cohavora_import_release_archive(cohavora_dep_angle
        "${qt_LIBRARIES_ROOT}/tg_angle/out/Release/tg_angle.lib")
    cohavora_import_release_archive(cohavora_dep_zlib
        "${qt_LIBRARIES_ROOT}/zlib/Release/zlibstatic.lib")
    cohavora_import_release_archive(cohavora_dep_mozjpeg
        "${qt_LIBRARIES_ROOT}/mozjpeg/release/jpeg-static.lib")

    cohavora_import_release_archive(cohavora_dep_qt_core
        "${qt_QT_ROOT}/lib/Qt5Core.lib")
    cohavora_import_release_archive(cohavora_dep_qt_gui
        "${qt_QT_ROOT}/lib/Qt5Gui.lib")
    cohavora_import_release_archive(cohavora_dep_qt_widgets
        "${qt_QT_ROOT}/lib/Qt5Widgets.lib")
    cohavora_import_release_archive(cohavora_dep_qt_network
        "${qt_QT_ROOT}/lib/Qt5Network.lib")
    cohavora_import_release_archive(cohavora_dep_qt_svg
        "${qt_QT_ROOT}/lib/Qt5Svg.lib")
    cohavora_import_release_archive(cohavora_dep_qt_font_database_support
        "${qt_QT_ROOT}/lib/Qt5FontDatabaseSupport.lib")
    cohavora_import_release_archive(cohavora_dep_qt_event_dispatcher_support
        "${qt_QT_ROOT}/lib/Qt5EventDispatcherSupport.lib")
    cohavora_import_release_archive(cohavora_dep_qt_theme_support
        "${qt_QT_ROOT}/lib/Qt5ThemeSupport.lib")
    cohavora_import_release_archive(cohavora_dep_qt_windows_ui_automation_support
        "${qt_QT_ROOT}/lib/Qt5WindowsUIAutomationSupport.lib")
    cohavora_import_release_archive(cohavora_dep_qt_accessibility_support
        "${qt_QT_ROOT}/lib/Qt5AccessibilitySupport.lib")
    cohavora_import_release_archive(cohavora_dep_qt_device_discovery_support
        "${qt_QT_ROOT}/lib/Qt5DeviceDiscoverySupport.lib")
    cohavora_import_release_archive(cohavora_dep_qt_edid_support
        "${qt_QT_ROOT}/lib/Qt5EdidSupport.lib")
    cohavora_import_release_archive(cohavora_dep_qt_freetype
        "${qt_QT_ROOT}/lib/qtfreetype.lib")
    cohavora_import_release_archive(cohavora_dep_qt_harfbuzz
        "${qt_QT_ROOT}/lib/qtharfbuzz.lib")
    cohavora_import_release_archive(cohavora_dep_qt_png
        "${qt_QT_ROOT}/lib/qtlibpng.lib")
    cohavora_import_release_archive(cohavora_dep_qt_pcre2
        "${qt_QT_ROOT}/lib/qtpcre2.lib")
    cohavora_import_release_archive(cohavora_dep_qt_windows_plugin
        "${qt_QT_ROOT}/plugins/platforms/qwindows.lib")
    cohavora_import_release_archive(cohavora_dep_qt_windows_vista_style_plugin
        "${qt_QT_ROOT}/plugins/styles/qwindowsvistastyle.lib")
    cohavora_import_release_archive(cohavora_dep_qt_svg_plugin
        "${qt_QT_ROOT}/plugins/imageformats/qsvg.lib")
    cohavora_import_release_archive(cohavora_dep_qt_ico_plugin
        "${qt_QT_ROOT}/plugins/imageformats/qico.lib")
    cohavora_import_release_archive(cohavora_dep_qt_jpeg_plugin
        "${qt_QT_ROOT}/plugins/imageformats/qjpeg.lib")
    cohavora_import_release_archive(cohavora_dep_qt_gif_plugin
        "${qt_QT_ROOT}/plugins/imageformats/qgif.lib")
    cohavora_import_release_archive(cohavora_dep_qt_tga_plugin
        "${qt_QT_ROOT}/plugins/imageformats/qtga.lib")
    cohavora_import_release_archive(cohavora_dep_qt_tiff_plugin
        "${qt_QT_ROOT}/plugins/imageformats/qtiff.lib")
    cohavora_import_release_archive(cohavora_dep_qt_wbmp_plugin
        "${qt_QT_ROOT}/plugins/imageformats/qwbmp.lib")
    cohavora_import_release_archive(cohavora_dep_qt_webp_plugin
        "${qt_QT_ROOT}/plugins/imageformats/qwebp.lib")
    cohavora_import_release_archive(cohavora_dep_qt_svg_icon_plugin
        "${qt_QT_ROOT}/plugins/iconengines/qsvgicon.lib")

    add_library(cohavora_qt_core_runtime INTERFACE)
    add_library(cohavora::qt_core_runtime ALIAS cohavora_qt_core_runtime)
    cohavora_copy_qt_compile_usage(cohavora_qt_core_runtime Qt5::Core)
    target_link_libraries(cohavora_qt_core_runtime INTERFACE
        cohavora_dep_qt_core
        cohavora_dep_qt_pcre2
        netapi32 userenv version winmm)

    add_library(cohavora_qt_network_runtime INTERFACE)
    add_library(cohavora::qt_network_runtime ALIAS cohavora_qt_network_runtime)
    cohavora_copy_qt_compile_usage(cohavora_qt_network_runtime Qt5::Network)
    target_link_libraries(cohavora_qt_network_runtime INTERFACE
        cohavora_qt_core_runtime
        cohavora_dep_qt_network
        ws2_32 crypt32 dnsapi iphlpapi secur32)

    add_library(cohavora_qt_gui_runtime INTERFACE)
    add_library(cohavora::qt_gui_runtime ALIAS cohavora_qt_gui_runtime)
    cohavora_copy_qt_compile_usage(cohavora_qt_gui_runtime Qt5::Gui)
    target_link_libraries(cohavora_qt_gui_runtime INTERFACE
        cohavora_qt_core_runtime
        cohavora_dep_qt_gui
        cohavora_dep_qt_freetype
        cohavora_dep_qt_harfbuzz
        cohavora_dep_qt_png
        imm32 wtsapi32 ws2_32 crypt32)

    add_library(cohavora_qt_widgets_runtime INTERFACE)
    add_library(cohavora::qt_widgets_runtime ALIAS cohavora_qt_widgets_runtime)
    cohavora_copy_qt_compile_usage(cohavora_qt_widgets_runtime Qt5::Widgets)
    target_link_libraries(cohavora_qt_widgets_runtime INTERFACE
        cohavora_qt_gui_runtime
        cohavora_qt_network_runtime
        cohavora_dep_qt_widgets
        cohavora_dep_zlib)

    add_library(cohavora_qt_svg_runtime INTERFACE)
    add_library(cohavora::qt_svg_runtime ALIAS cohavora_qt_svg_runtime)
    cohavora_copy_qt_compile_usage(cohavora_qt_svg_runtime Qt5::Svg)
    target_link_libraries(cohavora_qt_svg_runtime INTERFACE
        cohavora_qt_widgets_runtime
        cohavora_dep_qt_svg)

    add_library(cohavora_qt_application_runtime INTERFACE)
    add_library(cohavora::qt_application_runtime ALIAS
        cohavora_qt_application_runtime)
    target_link_libraries(cohavora_qt_application_runtime INTERFACE
        cohavora_qt_svg_runtime
        cohavora_dep_angle
        cohavora_dep_qt_font_database_support
        cohavora_dep_qt_event_dispatcher_support
        cohavora_dep_qt_theme_support
        cohavora_dep_qt_windows_ui_automation_support
        cohavora_dep_qt_accessibility_support
        cohavora_dep_qt_device_discovery_support
        cohavora_dep_qt_edid_support
        cohavora_dep_qt_windows_plugin
        cohavora_dep_qt_windows_vista_style_plugin
        cohavora_dep_qt_svg_plugin
        cohavora_dep_qt_ico_plugin
        cohavora_dep_qt_jpeg_plugin
        cohavora_dep_qt_gif_plugin
        cohavora_dep_qt_tga_plugin
        cohavora_dep_qt_tiff_plugin
        cohavora_dep_qt_wbmp_plugin
        cohavora_dep_qt_webp_plugin
        cohavora_dep_qt_svg_icon_plugin
        cohavora_dep_mozjpeg
        Dwmapi UxTheme d3d9 dxgi d3d11 d3dcompiler dxguid)

    message(STATUS
        "[Qt] Using pinned ${COHAVORA_QT_VERSION} Release ABI package: ${qt_QT_ROOT}")
endmacro()

function(cohavora_define_desktop_ui_runtime)
    if(TARGET cohavora_desktop_ui_runtime)
        return()
    endif()
    foreach(required_target IN ITEMS
            cohavora_qt_application_runtime
            desktop_lib_ui desktop_lib_base desktop_lib_crl desktop_lz4)
        if(NOT TARGET ${required_target})
            message(FATAL_ERROR
                "cohavora_define_desktop_ui_runtime requires ${required_target}")
        endif()
    endforeach()

    add_library(cohavora_desktop_ui_runtime INTERFACE)
    add_library(cohavora::desktop_ui_runtime ALIAS cohavora_desktop_ui_runtime)
    target_include_directories(cohavora_desktop_ui_runtime BEFORE INTERFACE
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_ui
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/gen
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/gen/styles
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_base
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_rpl
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_crl
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/GSL/include
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/range-v3/include
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/expected/include
        ${PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/xxHash
        ${QT_PATH}/include/QtCore/5.15.18
        ${QT_PATH}/include/QtCore/5.15.18/QtCore
        ${QT_PATH}/include/QtGui/5.15.18
        ${QT_PATH}/include/QtGui/5.15.18/QtGui
        ${TDESKTOP_LIBS_DIR}/tg_angle/include
        ${TDESKTOP_LIBS_DIR}/zlib/include)
    target_link_libraries(cohavora_desktop_ui_runtime INTERFACE
        cohavora_qt_application_runtime
        desktop_lib_ui
        desktop_lib_base
        desktop_lib_crl
        desktop_lz4
        ${CMAKE_DL_LIBS})
endfunction()
