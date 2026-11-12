# Telegram Desktop UI Core Framework (Native CMake Subproject)
# Self-contained pure C++ implementation for LiveKit Client UI

cohavora_group_qt_generated_sources()

set(TDESKTOP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Select platform sources in this product wrapper; the pinned toolkit submodules
# stay unmodified. Qt's own private targets carry the matching SDK include paths.
if(UNIX AND NOT APPLE)
    find_package(Qt5 ${COHAVORA_QT_VERSION} EXACT CONFIG REQUIRED COMPONENTS DBus)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TDESKTOP_GIO REQUIRED IMPORTED_TARGET gio-2.0)
    set(TDESKTOP_PLATFORM_DEFINITIONS CRL_USE_QT)
    set(TDESKTOP_PLATFORM_LIBRARIES ${CMAKE_DL_LIBS})
else()
    set(TDESKTOP_PLATFORM_DEFINITIONS
        WIN32 _WINDOWS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX CRL_USE_WINAPI)
    set(TDESKTOP_PLATFORM_LIBRARIES Dwmapi UxTheme Version)
endif()

# --- 1. desktop_lz4 (lz4 & xxHash) ---
add_library(desktop_lz4 STATIC
    ThirdParty/lz4/lib/lz4.c
    ThirdParty/lz4/lib/lz4frame.c
    ThirdParty/lz4/lib/lz4hc.c
    ThirdParty/xxHash/xxhash.c
)
target_include_directories(desktop_lz4 PUBLIC
    ThirdParty/lz4/lib
    ThirdParty/xxHash
)

# --- 2. desktop_lib_crl (Concurrency Runtime Library) ---
file(GLOB_RECURSE CRL_COMMON_SOURCES CONFIGURE_DEPENDS
    "lib_crl/crl/common/*.cpp")
file(GLOB_RECURSE CRL_DISPATCH_SOURCES CONFIGURE_DEPENDS
    "lib_crl/crl/dispatch/*.cpp")
if(WIN32)
    file(GLOB_RECURSE CRL_PLATFORM_SOURCES CONFIGURE_DEPENDS
        "lib_crl/crl/winapi/*.cpp")
else()
    set(CRL_PLATFORM_SOURCES lib_crl/crl/linux/crl_linux_time.cpp)
endif()

add_library(desktop_lib_crl STATIC
    ${CRL_COMMON_SOURCES}
    ${CRL_DISPATCH_SOURCES}
    ${CRL_PLATFORM_SOURCES}
    lib_crl/crl/crl_object_on_thread.cpp
    lib_crl/crl/crl_time.cpp
)

target_include_directories(desktop_lib_crl PUBLIC
    ${TDESKTOP_SRC_DIR}/lib_crl
    ${TDESKTOP_SRC_DIR}/lib_rpl
    ${TDESKTOP_SRC_DIR}/lib_base
    ${TDESKTOP_SRC_DIR}/ThirdParty/GSL/include
)

target_compile_definitions(desktop_lib_crl PUBLIC ${TDESKTOP_PLATFORM_DEFINITIONS})
target_link_libraries(desktop_lib_crl PUBLIC Qt5::Core)

# --- 3. desktop_lib_base (Base Utilities & Win Platform) ---
file(GLOB BASE_SOURCES CONFIGURE_DEPENDS "lib_base/base/*.cpp")
list(FILTER BASE_SOURCES EXCLUDE REGEX "crash_report_.*\\.cpp$")
list(FILTER BASE_SOURCES EXCLUDE REGEX "_tests?\\.cpp$")
list(FILTER BASE_SOURCES EXCLUDE REGEX "tests_.*\\.cpp$")
if(WIN32)
    list(FILTER BASE_SOURCES EXCLUDE REGEX ".*_posix\\.cpp$")
    file(GLOB BASE_PLATFORM_SOURCES CONFIGURE_DEPENDS
        "lib_base/base/platform/win/*.cpp")
else()
    list(FILTER BASE_SOURCES EXCLUDE REGEX ".*_win\\.cpp$")
    # These toolkit Linux files only use Qt 5 or public system APIs. The other
    # Linux files require the upstream GI C++ generator / Qt 6 integration;
    # product adapters below implement the subset consumed by the shared UI.
    set(BASE_PLATFORM_SOURCES
        lib_base/base/platform/linux/base_battery_saving_linux.cpp
        lib_base/base/platform/linux/base_global_shortcuts_linux.cpp
        lib_base/base/platform/linux/base_info_linux.cpp
        lib_base/base/platform/linux/base_layout_switch_linux.cpp
        lib_base/base/platform/linux/base_linux_allocation_tracer.cpp
        lib_base/base/platform/linux/base_linux_library.cpp
        lib_base/base/platform/linux/base_linux_xcb_library.cpp
        lib_base/base/platform/linux/base_linux_xcb_utilities.cpp
        lib_base/base/platform/linux/base_linux_xsettings.cpp
        lib_base/base/platform/linux/base_network_reachability_linux.cpp
        lib_base/base/platform/linux/base_process_linux.cpp
        lib_base/base/platform/linux/base_system_unlock_linux.cpp
        linux/qt5_platform.cpp
        linux/qt5_screen_reader.cpp)
endif()

add_library(desktop_lib_base STATIC
    ${BASE_SOURCES}
    ${BASE_PLATFORM_SOURCES}
    lib_base/base/platform/base_platform_info.cpp
)

target_precompile_headers(desktop_lib_base PRIVATE ${TDESKTOP_SRC_DIR}/lib_base/base/base_pch.h)

target_include_directories(desktop_lib_base PUBLIC
    ${TDESKTOP_SRC_DIR}/lib_base
    ${TDESKTOP_SRC_DIR}/lib_crl
    ${TDESKTOP_SRC_DIR}/lib_rpl
    ${TDESKTOP_SRC_DIR}/ThirdParty/GSL/include
    ${TDESKTOP_SRC_DIR}/ThirdParty/range-v3/include
    ${TDESKTOP_SRC_DIR}/ThirdParty/expected/include
    ${TDESKTOP_SRC_DIR}/ThirdParty/xxHash
)

target_link_libraries(desktop_lib_base PUBLIC
    desktop_lib_crl
    desktop_lz4
    OpenSSL::Crypto
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    ${TDESKTOP_PLATFORM_LIBRARIES}
)
if(UNIX AND NOT APPLE)
    target_compile_definitions(desktop_lib_base PRIVATE QT_NO_KEYWORDS)
    target_link_libraries(desktop_lib_base PRIVATE Qt5::GuiPrivate Qt5::DBus PkgConfig::TDESKTOP_GIO)
endif()

target_compile_definitions(desktop_lib_base PUBLIC ${TDESKTOP_PLATFORM_DEFINITIONS})

# --- 4. desktop_lib_ui (UI Widgets, Styles & Palette) ---
file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS "lib_ui/ui/*.cpp")
file(GLOB_RECURSE UI_EMOJI_SOURCES CONFIGURE_DEPENDS
    "lib_ui/emoji_suggestions/*.cpp")
list(FILTER UI_SOURCES EXCLUDE REGEX "tests_.*\\.cpp$")
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/platform/mac/.*")
if(WIN32)
    list(FILTER UI_SOURCES EXCLUDE REGEX "ui/platform/linux/.*")
else()
    list(FILTER UI_SOURCES EXCLUDE REGEX "ui/platform/win/.*")
    list(FILTER UI_SOURCES EXCLUDE REGEX "ui/platform/linux/ui_window_title_linux\\.cpp$")
    list(APPEND UI_SOURCES linux/qt5_window_title.cpp)
endif()

file(GLOB_RECURSE UI_GEN_SOURCES CONFIGURE_DEPENDS "gen/*.cpp")

# Qt Resources (fonts and windows theme icons)
set(UI_RESOURCES lib_ui/fonts/fonts.qrc)
if(WIN32)
    list(APPEND UI_RESOURCES lib_ui/qt_conf/win.qrc)
endif()

qt5_add_resources(UI_QRC_SOURCES ${UI_RESOURCES})

add_library(desktop_lib_ui STATIC
    ${UI_SOURCES}
    ${UI_EMOJI_SOURCES}
    ${UI_GEN_SOURCES}
    ${UI_QRC_SOURCES}
)

target_precompile_headers(desktop_lib_ui PRIVATE ${TDESKTOP_SRC_DIR}/lib_ui/ui/ui_pch.h)
if(MSVC)
    target_compile_options(desktop_lib_ui PRIVATE /FI${TDESKTOP_SRC_DIR}/qt5_compat.h)
else()
    target_compile_options(desktop_lib_ui PRIVATE -include ${TDESKTOP_SRC_DIR}/qt5_compat.h)
    target_include_directories(desktop_lib_ui BEFORE PRIVATE ${TDESKTOP_SRC_DIR}/linux/include)
endif()

target_include_directories(desktop_lib_ui PUBLIC
    ${TDESKTOP_SRC_DIR}/lib_ui
    ${TDESKTOP_SRC_DIR}/gen
    ${TDESKTOP_SRC_DIR}/gen/styles
    ${TDESKTOP_SRC_DIR}/lib_ui/emoji_suggestions
    ${TDESKTOP_SRC_DIR}/lib_base
    ${TDESKTOP_SRC_DIR}/lib_crl
    ${TDESKTOP_SRC_DIR}/lib_rpl
    ${TDESKTOP_SRC_DIR}/ThirdParty/GSL/include
    ${TDESKTOP_SRC_DIR}/ThirdParty/range-v3/include
    ${TDESKTOP_SRC_DIR}/ThirdParty/expected/include
    ${TDESKTOP_SRC_DIR}/ThirdParty/xxHash

)

if(WIN32)
    target_include_directories(desktop_lib_ui PUBLIC
    ${TDESKTOP_LIBS_DIR}/tg_angle/include
    ${TDESKTOP_LIBS_DIR}/mozjpeg
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui/5.15.18/QtGui
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore/5.15.18/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtWidgets/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtWidgets/5.15.18/QtWidgets
    )
else()
    # Consume the JPEG headers and archives built with this exact static Qt.
    # A distro JPEG header may describe a different JPEG_LIB_VERSION ABI.
    target_include_directories(desktop_lib_ui PRIVATE ${QT_PATH}/include/qtlibjpeg)
    target_link_libraries(desktop_lib_ui PRIVATE
        ${QT_PATH}/lib/libqtlibjpeg.a
        ${QT_PATH}/lib/libqtlibjpeg12bits.a
        ${QT_PATH}/lib/libqtlibjpeg16bits.a
        ZLIB::ZLIB)
endif()

target_link_libraries(desktop_lib_ui PUBLIC
    desktop_lib_base
    desktop_lib_crl
    desktop_lz4
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Svg
    ${TDESKTOP_PLATFORM_LIBRARIES}
)
if(WIN32)
    target_link_libraries(desktop_lib_ui PUBLIC d3d9 dxgi d3d11 dxguid)
else()
    target_link_libraries(desktop_lib_ui PUBLIC Qt5::CorePrivate Qt5::GuiPrivate Qt5::WidgetsPrivate)
    target_link_libraries(desktop_lib_ui PRIVATE Qt5::DBus)
endif()

target_compile_definitions(desktop_lib_ui PUBLIC ${TDESKTOP_PLATFORM_DEFINITIONS})
