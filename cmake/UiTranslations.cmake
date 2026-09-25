# The application can display its source language without Linguist tools.
# Release configurations can opt into a strict, reproducible resource check.
set(COHAVORA_LUPDATE_EXECUTABLE "" CACHE FILEPATH
    "Explicit lupdate executable; empty uses the selected Qt SDK")
set(COHAVORA_LRELEASE_EXECUTABLE "" CACHE FILEPATH
    "Explicit lrelease executable; empty uses the selected Qt SDK")
set(COHAVORA_QT_ZH_CN_TRANSLATION "" CACHE FILEPATH
    "Explicit qtbase_zh_CN.qm; empty uses the selected Qt SDK")
option(COHAVORA_REQUIRE_UI_TRANSLATIONS
    "Require matching Linguist tools and embedded UI/Qt catalogs" OFF)
mark_as_advanced(
    COHAVORA_LUPDATE_EXECUTABLE
    COHAVORA_LRELEASE_EXECUTABLE
    COHAVORA_QT_ZH_CN_TRANSLATION)

# Clear outputs left by the old cached find calls. They cannot be distinguished
# from explicit values and may point at a different Qt SDK found through PATH.
foreach(legacy_variable IN ITEMS
        LIVEKIT_LUPDATE_EXECUTABLE
        LIVEKIT_LRELEASE_EXECUTABLE
        LIVEKIT_QT_ZH_CN_TRANSLATION)
    set(legacy_value "${${legacy_variable}}")
    if(legacy_value)
        message(DEPRECATION
            "Ignoring deprecated cached ${legacy_variable}=${legacy_value}. "
            "Use the corresponding COHAVORA_* override if it is intentional.")
    endif()
    unset(${legacy_variable} CACHE)
endforeach()

function(cohavora_resolve_qt_linguist_tool output override_variable)
    set(multi_value_args NAMES)
    cmake_parse_arguments(TOOL "" "" "${multi_value_args}" ${ARGN})

    unset(executable)
    set(explicit_path "${${override_variable}}")
    if(explicit_path)
        get_filename_component(executable "${explicit_path}" ABSOLUTE
            BASE_DIR "${CMAKE_SOURCE_DIR}")
        if(NOT EXISTS "${executable}")
            message(FATAL_ERROR
                "${override_variable} does not exist: ${executable}")
        endif()
    else()
        find_program(executable
            NAMES ${TOOL_NAMES}
            PATHS "${QT_PATH}/bin"
            NO_DEFAULT_PATH
            NO_CACHE)
    endif()

    if(executable)
        execute_process(
            COMMAND "${executable}" -version
            RESULT_VARIABLE version_result
            OUTPUT_VARIABLE version_stdout
            ERROR_VARIABLE version_stderr
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_STRIP_TRAILING_WHITESPACE)
        set(version_output "${version_stdout}\n${version_stderr}")
        if(NOT version_result EQUAL 0)
            message(FATAL_ERROR
                "Unable to query Qt Linguist tool version: ${executable}\n"
                "${version_output}")
        endif()
        string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" tool_version
            "${version_output}")
        if(NOT tool_version)
            message(FATAL_ERROR
                "Unable to parse Qt Linguist tool version: ${executable}\n"
                "${version_output}")
        endif()
        if(Qt5Core_VERSION_STRING
                AND NOT tool_version VERSION_EQUAL Qt5Core_VERSION_STRING)
            message(FATAL_ERROR
                "Qt Linguist tool ${executable} is version ${tool_version}, but "
                "the selected Qt SDK is ${Qt5Core_VERSION_STRING}.")
        endif()
        message(STATUS
            "Using Qt Linguist ${TOOL_NAMES} ${tool_version}: ${executable}")
    endif()

    set(${output} "${executable}" PARENT_SCOPE)
endfunction()

function(cohavora_xml_escape output value)
    set(escaped "${value}")
    string(REPLACE "&" "&amp;" escaped "${escaped}")
    string(REPLACE "<" "&lt;" escaped "${escaped}")
    string(REPLACE ">" "&gt;" escaped "${escaped}")
    string(REPLACE "\"" "&quot;" escaped "${escaped}")
    string(REPLACE "'" "&apos;" escaped "${escaped}")
    set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

function(livekit_add_ui_translations target)
    set(one_value_args SOURCE_ROOT)
    cmake_parse_arguments(LIVEKIT_TRANSLATIONS "" "${one_value_args}" "" ${ARGN})
    if(NOT LIVEKIT_TRANSLATIONS_SOURCE_ROOT)
        message(FATAL_ERROR "livekit_add_ui_translations requires SOURCE_ROOT")
    endif()

    set(source_root "${LIVEKIT_TRANSLATIONS_SOURCE_ROOT}")
    cohavora_resolve_qt_linguist_tool(lupdate_executable
        COHAVORA_LUPDATE_EXECUTABLE NAMES lupdate lupdate-qt5)
    cohavora_resolve_qt_linguist_tool(lrelease_executable
        COHAVORA_LRELEASE_EXECUTABLE NAMES lrelease lrelease-qt5)

    file(GLOB ui_translation_sources CONFIGURE_DEPENDS
        "${source_root}/src/ui/*.cpp"
        "${source_root}/src/ui/*.h"
        "${source_root}/src/ui/render/*.cpp"
        "${source_root}/src/ui/whiteboard/*.cpp"
        "${source_root}/src/ui/whiteboard/*.h"
        "${source_root}/src/net/*.cpp"
        "${source_root}/src/net/*.h")
    list(APPEND ui_translation_sources
        "${source_root}/src/app/main_meeting_app.cpp"
        "${source_root}/src/core/meeting_coordinator.cpp"
        "${source_root}/src/core/meeting_catalog_controller.cpp")
    file(GLOB ui_translation_catalogs CONFIGURE_DEPENDS
        "${source_root}/src/ui/translations/cohavora_*.ts")

    if(COHAVORA_REQUIRE_UI_TRANSLATIONS AND NOT ui_translation_catalogs)
        message(FATAL_ERROR
            "No Cohavora UI translation catalogs were found for the strict release build")
    endif()

    if(lupdate_executable)
        add_custom_target(update_ui_translations
            COMMAND "${lupdate_executable}" ${ui_translation_sources}
                -no-obsolete -source-language en_US -ts ${ui_translation_catalogs}
            COMMENT "Updating UI translation catalogs" VERBATIM)
    endif()

    set(translation_sources
        "${source_root}/src/ui/app_translation.cpp"
        "${source_root}/src/ui/app_translation.h")
    set(has_embedded_translations FALSE)
    if(lrelease_executable AND ui_translation_catalogs)
        set(ui_translation_resource "<RCC><qresource prefix=\"/meeting-ui/translations\">\n")
        # qtbase is self-contained. Older qt_zh_CN catalogs may lack Qt 5 contexts,
        # while newer qt catalogs are manifests with external module dependencies.
        unset(qt_zh_cn_translation)
        if(COHAVORA_QT_ZH_CN_TRANSLATION)
            get_filename_component(qt_zh_cn_translation
                "${COHAVORA_QT_ZH_CN_TRANSLATION}" ABSOLUTE
                BASE_DIR "${CMAKE_SOURCE_DIR}")
            if(NOT EXISTS "${qt_zh_cn_translation}")
                message(FATAL_ERROR
                    "COHAVORA_QT_ZH_CN_TRANSLATION does not exist: "
                    "${qt_zh_cn_translation}")
            endif()
        else()
            find_file(qt_zh_cn_translation NAMES qtbase_zh_CN.qm
                PATHS "${QT_PATH}/translations"
                NO_DEFAULT_PATH
                NO_CACHE)
        endif()
        if(qt_zh_cn_translation)
            cohavora_xml_escape(qt_zh_cn_translation_xml
                "${qt_zh_cn_translation}")
            string(APPEND ui_translation_resource
                "<file alias=\"qt_zh_CN.qm\">${qt_zh_cn_translation_xml}</file>\n")
        elseif(COHAVORA_REQUIRE_UI_TRANSLATIONS)
            message(FATAL_ERROR
                "Qt Chinese catalog is required but missing from ${QT_PATH}/translations")
        else()
            message(WARNING "Qt Chinese catalog not found; standard controls will use source text")
        endif()
        foreach(catalog IN LISTS ui_translation_catalogs)
            get_filename_component(name "${catalog}" NAME_WE)
            set(qm "${CMAKE_CURRENT_BINARY_DIR}/translations/${name}.qm")
            add_custom_command(OUTPUT "${qm}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/translations"
                COMMAND "${lrelease_executable}" "${catalog}" -qm "${qm}"
                DEPENDS "${catalog}" "${lrelease_executable}"
                VERBATIM)
            cohavora_xml_escape(name_xml "${name}.qm")
            cohavora_xml_escape(qm_xml "${qm}")
            string(APPEND ui_translation_resource
                "<file alias=\"${name_xml}\">${qm_xml}</file>\n")
        endforeach()
        string(APPEND ui_translation_resource "</qresource></RCC>\n")
        set(ui_translation_qrc "${CMAKE_CURRENT_BINARY_DIR}/ui_translations.qrc")
        configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/UiTranslations.qrc.in"
            "${ui_translation_qrc}"
            @ONLY)
        qt5_add_resources(ui_translation_resources "${ui_translation_qrc}")
        list(APPEND translation_sources ${ui_translation_resources})
        set(has_embedded_translations TRUE)
    elseif(COHAVORA_REQUIRE_UI_TRANSLATIONS)
        message(FATAL_ERROR
            "A matching lrelease executable is required for strict UI translations. "
            "Install Qt Linguist in ${QT_PATH}/bin or set "
            "COHAVORA_LRELEASE_EXECUTABLE.")
    else()
        message(STATUS "lrelease not found: using source UI text; external translation catalogs can still be loaded")
    endif()

    add_library(${target} OBJECT ${translation_sources})
    target_include_directories(${target} PUBLIC "${source_root}")
    target_link_libraries(${target} PRIVATE Qt5::Core)
    set_target_properties(${target} PROPERTIES
        AUTOMOC OFF
        COHAVORA_HAS_EMBEDDED_TRANSLATIONS "${has_embedded_translations}"
        FOLDER "resources")
endfunction()
