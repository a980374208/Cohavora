# Telegram Desktop UI Core Framework (Native CMake Subproject)
# Self-contained pure C++ implementation for LiveKit Client UI

set(TDESKTOP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

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
file(GLOB_RECURSE CRL_COMMON_SOURCES "lib_crl/crl/common/*.cpp")
file(GLOB_RECURSE CRL_DISPATCH_SOURCES "lib_crl/crl/dispatch/*.cpp")
file(GLOB_RECURSE CRL_WINAPI_SOURCES "lib_crl/crl/winapi/*.cpp")

add_library(desktop_lib_crl STATIC
    ${CRL_COMMON_SOURCES}
    ${CRL_DISPATCH_SOURCES}
    ${CRL_WINAPI_SOURCES}
    lib_crl/crl/crl_object_on_thread.cpp
    lib_crl/crl/crl_time.cpp
)

target_include_directories(desktop_lib_crl PUBLIC
    ${TDESKTOP_SRC_DIR}/lib_crl
    ${TDESKTOP_SRC_DIR}/lib_rpl
    ${TDESKTOP_SRC_DIR}/lib_base
    ${TDESKTOP_SRC_DIR}/ThirdParty/GSL/include
)

target_compile_definitions(desktop_lib_crl PUBLIC
    WIN32
    _WINDOWS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    CRL_USE_WINAPI
)

# --- 3. desktop_lib_base (Base Utilities & Win Platform) ---
file(GLOB BASE_SOURCES "lib_base/base/*.cpp")
list(FILTER BASE_SOURCES EXCLUDE REGEX "crash_report_.*\\.cpp$")
list(FILTER BASE_SOURCES EXCLUDE REGEX "_tests?\\.cpp$")
list(FILTER BASE_SOURCES EXCLUDE REGEX "tests_.*\\.cpp$")
list(FILTER BASE_SOURCES EXCLUDE REGEX ".*_posix\\.cpp$")

file(GLOB BASE_WIN_SOURCES "lib_base/base/platform/win/*.cpp")

add_library(desktop_lib_base STATIC
    ${BASE_SOURCES}
    ${BASE_WIN_SOURCES}
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
    Dwmapi
    UxTheme
    Version
)

target_compile_definitions(desktop_lib_base PUBLIC
    WIN32
    _WINDOWS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH
    _ALLOW_RUNTIME_LIBRARY_MISMATCH
)

# --- 4. desktop_lib_ui (UI Widgets, Styles & Palette) ---
file(GLOB_RECURSE UI_SOURCES "lib_ui/ui/*.cpp")
file(GLOB_RECURSE UI_EMOJI_SOURCES "lib_ui/emoji_suggestions/*.cpp")
list(FILTER UI_SOURCES EXCLUDE REGEX "tests_.*\\.cpp$")
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/platform/linux/.*")
list(FILTER UI_SOURCES EXCLUDE REGEX "ui/platform/mac/.*")

file(GLOB_RECURSE UI_GEN_SOURCES "gen/*.cpp")

# Qt Resources (fonts and windows theme icons)
set(UI_RESOURCES
    lib_ui/fonts/fonts.qrc
    lib_ui/qt_conf/win.qrc
)

qt5_add_resources(UI_QRC_SOURCES ${UI_RESOURCES})

add_library(desktop_lib_ui STATIC
    ${UI_SOURCES}
    ${UI_EMOJI_SOURCES}
    ${UI_GEN_SOURCES}
    ${UI_QRC_SOURCES}
)

target_precompile_headers(desktop_lib_ui PRIVATE ${TDESKTOP_SRC_DIR}/lib_ui/ui/ui_pch.h)
target_compile_options(desktop_lib_ui PRIVATE /FI${TDESKTOP_SRC_DIR}/qt5_compat.h)

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
    ${TDESKTOP_LIBS_DIR}/tg_angle/include
    ${TDESKTOP_LIBS_DIR}/mozjpeg
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui/5.15.18/QtGui
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore/5.15.18/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtWidgets/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtWidgets/5.15.18/QtWidgets
)

target_link_libraries(desktop_lib_ui PUBLIC
    desktop_lib_base
    desktop_lib_crl
    desktop_lz4
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Svg
    Dwmapi
    UxTheme
    d3d9
    dxgi
    d3d11
    dxguid
)

target_compile_definitions(desktop_lib_ui PUBLIC
    WIN32
    _WINDOWS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH
    _ALLOW_RUNTIME_LIBRARY_MISMATCH
)
