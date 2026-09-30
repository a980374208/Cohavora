if(NOT DEFINED TEST_EXECUTABLE)
    message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()

execute_process(COMMAND "${TEST_EXECUTABLE}" --pass
    RESULT_VARIABLE pass_result OUTPUT_VARIABLE pass_output ERROR_VARIABLE pass_error
    TIMEOUT 10)
if(NOT "${pass_result}" STREQUAL "0" OR
   NOT pass_output MATCHES "CHECK_PASS_SIDE_EFFECTS=1")
    message(FATAL_ERROR "Passing/side-effect child failed: ${pass_result}\n${pass_output}\n${pass_error}")
endif()

execute_process(COMMAND "${TEST_EXECUTABLE}" --fail
    RESULT_VARIABLE fail_result OUTPUT_VARIABLE fail_output ERROR_VARIABLE fail_error
    TIMEOUT 10)
if(NOT "${fail_result}" STREQUAL "1" OR
   NOT fail_error MATCHES "TEST_CHECK failed: false")
    message(FATAL_ERROR "False-check child was not the expected failure: ${fail_result}\n${fail_output}\n${fail_error}")
endif()
message(STATUS "Always-active checks: true, exactly-once side effect, and intentional failure verified")
