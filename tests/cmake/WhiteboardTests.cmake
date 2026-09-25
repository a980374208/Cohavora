add_executable(test_whiteboard_document ${LIVEKIT_TEST_SOURCE_DIR}/whiteboard/test_whiteboard_document.cpp)
set_target_properties(test_whiteboard_document PROPERTIES AUTOMOC OFF)
target_link_libraries(test_whiteboard_document PRIVATE cohavora_whiteboard_model nlohmann_json::nlohmann_json)

add_executable(test_whiteboard_ui ${LIVEKIT_TEST_SOURCE_DIR}/whiteboard/test_whiteboard_ui.cpp)
livekit_configure_qt_test(test_whiteboard_ui)
target_link_libraries(test_whiteboard_ui PRIVATE cohavora_whiteboard_ui cohavora_ui_theme cohavora_ui_translations)
add_executable(test_whiteboard_collaboration
    ${LIVEKIT_TEST_SOURCE_DIR}/whiteboard/test_whiteboard_collaboration.cpp)
set_target_properties(test_whiteboard_collaboration PROPERTIES AUTOMOC OFF)
target_link_libraries(test_whiteboard_collaboration PRIVATE cohavora_whiteboard_model)
foreach(target IN ITEMS test_whiteboard_document test_whiteboard_ui test_whiteboard_collaboration)
    target_compile_options(${target} PRIVATE /utf-8)
endforeach()
foreach(target IN ITEMS test_participant_window_remediation test_camera_owner_remediation)
    target_link_libraries(${target} PRIVATE cohavora_whiteboard_ui)
endforeach()
add_test(NAME whiteboard_document_test COMMAND test_whiteboard_document)
add_test(NAME whiteboard_ui_test COMMAND test_whiteboard_ui)
add_test(NAME whiteboard_collaboration_test COMMAND test_whiteboard_collaboration)
add_test(NAME whiteboard_meeting_test COMMAND test_participant_window_remediation --whiteboard-meeting)
add_test(NAME whiteboard_collaboration_ui_test
    COMMAND test_participant_window_remediation --whiteboard-collaboration)
add_test(NAME screen_annotation_window_test
    COMMAND test_participant_window_remediation --screen-share)
set_tests_properties(whiteboard_document_test whiteboard_ui_test whiteboard_meeting_test PROPERTIES
    TIMEOUT 60 LABELS "WHITEBOARD_FOCUSED")
set_tests_properties(whiteboard_collaboration_test PROPERTIES
    TIMEOUT 60 LABELS "WHITEBOARD_COLLAB_FOCUSED")
set_tests_properties(whiteboard_ui_test whiteboard_collaboration_ui_test PROPERTIES
    TIMEOUT 60 RUN_SERIAL TRUE ENVIRONMENT "QT_SCALE_FACTOR=1")
set_property(TEST whiteboard_ui_test whiteboard_collaboration_ui_test
    APPEND PROPERTY LABELS "WHITEBOARD_COLLAB_FOCUSED")
set_tests_properties(whiteboard_ui_test whiteboard_meeting_test PROPERTIES
    RUN_SERIAL TRUE ENVIRONMENT "QT_SCALE_FACTOR=1")
set_tests_properties(screen_annotation_window_test PROPERTIES
    TIMEOUT 60 LABELS "ANNOTATION_FOCUSED" RUN_SERIAL TRUE ENVIRONMENT "QT_SCALE_FACTOR=1")
set_property(TEST screen_share_session_test whiteboard_ui_test
    APPEND PROPERTY LABELS "ANNOTATION_FOCUSED")
