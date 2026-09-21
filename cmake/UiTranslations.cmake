# The application can display its source language without Linguist tools.
# Set these cache paths when the Qt SDK omits the optional tools component.
find_program(LIVEKIT_LUPDATE_EXECUTABLE NAMES lupdate lupdate-qt5 HINTS "${QT_PATH}/bin")
find_program(LIVEKIT_LRELEASE_EXECUTABLE NAMES lrelease lrelease-qt5 HINTS "${QT_PATH}/bin")

file(GLOB ui_translation_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/render/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.h")
list(APPEND ui_translation_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/main_meeting_app.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/meeting_coordinator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/meeting_catalog_controller.cpp")
file(GLOB ui_translation_catalogs CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/translations/livekit_meeting_*.ts")
if(LIVEKIT_LUPDATE_EXECUTABLE)
    add_custom_target(update_ui_translations
        COMMAND "${LIVEKIT_LUPDATE_EXECUTABLE}" ${ui_translation_sources}
            -no-obsolete -source-language en_US -ts ${ui_translation_catalogs}
        COMMENT "Updating UI translation catalogs" VERBATIM)
endif()
if(LIVEKIT_LRELEASE_EXECUTABLE)
    set(ui_translation_resource "<RCC><qresource prefix=\"/meeting-ui/translations\">\n")
    get_filename_component(qt_linguist_bin "${LIVEKIT_LRELEASE_EXECUTABLE}" DIRECTORY)
    # qtbase is self-contained. Older qt_zh_CN catalogs may lack Qt 5 contexts,
    # while newer qt catalogs are manifests with external module dependencies.
    find_file(LIVEKIT_QT_ZH_CN_TRANSLATION NAMES qtbase_zh_CN.qm
        HINTS "${QT_PATH}/translations" "${qt_linguist_bin}/../translations")
    if(LIVEKIT_QT_ZH_CN_TRANSLATION)
        string(APPEND ui_translation_resource
            "<file alias=\"qt_zh_CN.qm\">${LIVEKIT_QT_ZH_CN_TRANSLATION}</file>\n")
    else()
        message(WARNING "Qt Chinese catalog not found; standard controls will use source text")
    endif()
    foreach(catalog IN LISTS ui_translation_catalogs)
        get_filename_component(name "${catalog}" NAME_WE)
        set(qm "${CMAKE_CURRENT_BINARY_DIR}/translations/${name}.qm")
        add_custom_command(OUTPUT "${qm}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/translations"
            COMMAND "${LIVEKIT_LRELEASE_EXECUTABLE}" "${catalog}" -qm "${qm}"
            DEPENDS "${catalog}" VERBATIM)
        string(APPEND ui_translation_resource "<file alias=\"${name}.qm\">${qm}</file>\n")
    endforeach()
    string(APPEND ui_translation_resource "</qresource></RCC>\n")
    set(ui_translation_qrc "${CMAKE_CURRENT_BINARY_DIR}/ui_translations.qrc")
    file(WRITE "${ui_translation_qrc}" "${ui_translation_resource}")
    qt5_add_resources(LIVEKIT_UI_TRANSLATION_RESOURCES "${ui_translation_qrc}")
else()
    message(STATUS "lrelease not found: using source UI text; external translation catalogs can still be loaded")
endif()
