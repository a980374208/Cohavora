set(LIVEKIT_PROJECT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")

add_subdirectory(tdesktop)
foreach(livekit_desktop_target IN ITEMS
        desktop_lz4
        desktop_lib_crl
        desktop_lib_base
        desktop_lib_ui)
    set_property(TARGET ${livekit_desktop_target} PROPERTY FOLDER "desktop")
endforeach()

qt5_add_resources(cohavora_ui_theme_resources app_theme.qrc)
add_library(cohavora_ui_theme OBJECT
    app_theme.cpp
    app_theme.h
    ${cohavora_ui_theme_resources})
target_include_directories(cohavora_ui_theme PUBLIC "${LIVEKIT_PROJECT_SOURCE_DIR}")
target_include_directories(cohavora_ui_theme PRIVATE
    ${QT_PATH}/include
    ${QT_PATH}/include/QtCore
    ${QT_PATH}/include/QtGui
    ${QT_PATH}/include/QtWidgets)
set_target_properties(cohavora_ui_theme PROPERTIES AUTOMOC OFF FOLDER "resources")

include("${LIVEKIT_PROJECT_SOURCE_DIR}/cmake/UiTranslations.cmake")
livekit_add_ui_translations(cohavora_ui_translations
    SOURCE_ROOT "${LIVEKIT_PROJECT_SOURCE_DIR}")
