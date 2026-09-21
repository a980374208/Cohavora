add_library(cohavora_qt_test_environment INTERFACE)
target_include_directories(cohavora_qt_test_environment BEFORE INTERFACE
    ${LIVEKIT_PROJECT_SOURCE_DIR}
    ${GEN_DIR}
    ${WEBRTC_ROOT}/include/third_party/libyuv/include
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_ui
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/tdesktop/gen
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/tdesktop/gen/styles
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_base
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_rpl
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/tdesktop/lib_crl
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/GSL/include
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/range-v3/include
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/expected/include
    ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/tdesktop/ThirdParty/xxHash
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtCore/5.15.18/QtCore
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui/5.15.18
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtGui/5.15.18/QtGui
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtWidgets
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/include/QtNetwork
    ${TDESKTOP_LIBS_DIR}/zlib/include)
target_link_libraries(cohavora_qt_test_environment INTERFACE
    ${CMAKE_DL_LIBS}
    desktop_lib_ui
    desktop_lib_base
    desktop_lib_crl
    desktop_lz4
    ${TDESKTOP_LIBS_DIR}/tg_angle/out/Release/tg_angle.lib
    ${TDESKTOP_LIBS_DIR}/zlib/Release/zlibstatic.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Core.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Gui.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Widgets.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Network.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5Svg.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5FontDatabaseSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5EventDispatcherSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5ThemeSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5WindowsUIAutomationSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5AccessibilitySupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5DeviceDiscoverySupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/Qt5EdidSupport.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtfreetype.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtharfbuzz.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtlibpng.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/lib/qtpcre2.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/platforms/qwindows.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/styles/qwindowsvistastyle.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qsvg.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qico.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qjpeg.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/imageformats/qgif.lib
    ${TDESKTOP_LIBS_DIR}/Qt-5.15.18/plugins/iconengines/qsvgicon.lib
    ${TDESKTOP_LIBS_DIR}/mozjpeg/release/jpeg-static.lib
    cohavora_core
    Dwmapi
    UxTheme
    d3d9
    dxgi
    d3d11
    d3dcompiler
    dxguid
    netapi32
    userenv
    imm32
    winmm
    wtsapi32
    version)
target_compile_definitions(cohavora_qt_test_environment INTERFACE
    WIN32
    _WINDOWS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WINSOCK_DEPRECATED_NO_WARNINGS
    CRL_USE_WINAPI
    ASIO_STANDALONE
    WEBRTC_WIN
    NDEBUG
    _ITERATOR_DEBUG_LEVEL=0
    $<$<CONFIG:Debug>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
    $<$<CONFIG:Debug>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>)
target_link_options(cohavora_qt_test_environment INTERFACE
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<CONFIG:Debug>:/NODEFAULTLIB:libcmtd.lib>)

function(livekit_configure_qt_test target)
    target_link_libraries(${target} PRIVATE cohavora_qt_test_environment)
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded")
endfunction()
