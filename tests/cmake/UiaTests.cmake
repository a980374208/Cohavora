if(WIN32)
    # Build on demand; never add an interactive test to ordinary CTest by default.
    add_executable(uia_console_fixture EXCLUDE_FROM_ALL
        ${LIVEKIT_TEST_SOURCE_DIR}/uia/console_fixture.cpp
        ${LIVEKIT_PROJECT_SOURCE_DIR}/src/ui/meeting_log_console.cpp)
    livekit_configure_qt_test(uia_console_fixture)
    target_link_libraries(uia_console_fixture PRIVATE cohavora_ui_theme cohavora_ui_translations)
    add_executable(uia_whiteboard_fixture EXCLUDE_FROM_ALL
        ${LIVEKIT_TEST_SOURCE_DIR}/uia/whiteboard_fixture.cpp)
    livekit_configure_qt_test(uia_whiteboard_fixture)
    target_link_libraries(uia_whiteboard_fixture PRIVATE
        cohavora_whiteboard_ui cohavora_ui_theme cohavora_ui_translations)
    add_executable(uia_whiteboard_clear_fixture EXCLUDE_FROM_ALL
        ${LIVEKIT_TEST_SOURCE_DIR}/uia/whiteboard_clear_fixture.cpp)
    livekit_configure_qt_test(uia_whiteboard_clear_fixture)
    target_link_libraries(uia_whiteboard_clear_fixture PRIVATE
        cohavora_whiteboard_ui cohavora_ui_theme cohavora_ui_translations)
    add_executable(uia_whiteboard_files_fixture EXCLUDE_FROM_ALL
        ${LIVEKIT_TEST_SOURCE_DIR}/uia/whiteboard_files_fixture.cpp)
    livekit_configure_qt_test(uia_whiteboard_files_fixture)
    target_link_libraries(uia_whiteboard_files_fixture PRIVATE
        cohavora_whiteboard_ui cohavora_ui_theme cohavora_ui_translations)
    get_target_property(_uia_app_sources cohavora_app SOURCES)
    set(_uia_entry_sources ${LIVEKIT_TEST_SOURCE_DIR}/uia/entry_fixture.cpp)
    foreach(_source IN LISTS _uia_app_sources)
        if(_source MATCHES "(^|/)main_meeting_app\\.cpp$" OR _source MATCHES "\\.rc$")
            continue()
        endif()
        if(IS_ABSOLUTE "${_source}")
            list(APPEND _uia_entry_sources "${_source}")
        else()
            list(APPEND _uia_entry_sources "${LIVEKIT_PROJECT_SOURCE_DIR}/src/app/${_source}")
        endif()
    endforeach()
    add_executable(uia_entry_fixture EXCLUDE_FROM_ALL ${_uia_entry_sources})
    livekit_configure_qt_test(uia_entry_fixture)
    get_target_property(_uia_app_includes cohavora_app INCLUDE_DIRECTORIES)
    get_target_property(_uia_app_links cohavora_app LINK_LIBRARIES)
    get_target_property(_uia_app_definitions cohavora_app COMPILE_DEFINITIONS)
    target_include_directories(uia_entry_fixture BEFORE PRIVATE ${_uia_app_includes})
    target_link_libraries(uia_entry_fixture PRIVATE ${_uia_app_links})
    target_compile_definitions(uia_entry_fixture PRIVATE ${_uia_app_definitions})
    set_target_properties(uia_entry_fixture PROPERTIES AUTOMOC ON)
    option(LIVEKIT_REGISTER_UIA_TESTS "Register opt-in interactive Windows UIA tests" OFF)
    if(LIVEKIT_REGISTER_UIA_TESTS)
        foreach(_scenario IN ITEMS join settings meeting)
            if(_scenario STREQUAL "meeting")
                set(_fixture test_participant_window_remediation)
            else()
                set(_fixture uia_entry_fixture)
            endif()
            add_test(NAME uia_${_scenario}_desktop_test COMMAND powershell.exe
                -NoProfile -ExecutionPolicy Bypass -File
                ${LIVEKIT_TEST_SOURCE_DIR}/uia/test_${_scenario}.ps1
                -Fixture $<TARGET_FILE:${_fixture}>
                -OutputDirectory ${CMAKE_BINARY_DIR}/uia-results)
            set_tests_properties(uia_${_scenario}_desktop_test PROPERTIES
                LABELS "UIA;INTERACTIVE_DESKTOP" TIMEOUT 120 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
        endforeach()
        add_test(NAME uia_whiteboard_files_desktop_test COMMAND powershell.exe
            -NoProfile -ExecutionPolicy Bypass -File
            ${LIVEKIT_TEST_SOURCE_DIR}/uia/test_whiteboard_files.ps1
            -Fixture $<TARGET_FILE:uia_whiteboard_files_fixture>
            -OutputDirectory ${CMAKE_BINARY_DIR}/uia-results)
        set_tests_properties(uia_whiteboard_files_desktop_test PROPERTIES
            LABELS "UIA;INTERACTIVE_DESKTOP" TIMEOUT 120 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
        add_test(NAME uia_whiteboard_clear_desktop_test COMMAND powershell.exe
            -NoProfile -ExecutionPolicy Bypass -File
            ${LIVEKIT_TEST_SOURCE_DIR}/uia/test_whiteboard_clear.ps1
            -Fixture $<TARGET_FILE:uia_whiteboard_clear_fixture>
            -OutputDirectory ${CMAKE_BINARY_DIR}/uia-results)
        set_tests_properties(uia_whiteboard_clear_desktop_test PROPERTIES
            LABELS "UIA;INTERACTIVE_DESKTOP" TIMEOUT 120 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
        add_test(NAME uia_console_desktop_test COMMAND powershell.exe
            -NoProfile -ExecutionPolicy Bypass -File
            ${LIVEKIT_TEST_SOURCE_DIR}/uia/test_console.ps1
            -Fixture $<TARGET_FILE:uia_console_fixture>
            -OutputDirectory ${CMAKE_BINARY_DIR}/uia-results)
        set_tests_properties(uia_console_desktop_test PROPERTIES
            LABELS "UIA;INTERACTIVE_DESKTOP" TIMEOUT 120 RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77)
        add_test(NAME uia_whiteboard_desktop_test COMMAND powershell.exe
            -NoProfile -ExecutionPolicy Bypass -File
            ${LIVEKIT_TEST_SOURCE_DIR}/uia/test_whiteboard.ps1
            -Fixture $<TARGET_FILE:uia_whiteboard_fixture>
            -OutputDirectory ${CMAKE_BINARY_DIR}/uia-results)
        set_tests_properties(uia_whiteboard_desktop_test PROPERTIES
            LABELS "UIA;INTERACTIVE_DESKTOP" TIMEOUT 120 RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77)
    endif()
endif()
