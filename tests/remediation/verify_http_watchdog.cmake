if(NOT DEFINED TEST_EXECUTABLE)
    message(FATAL_ERROR "TEST_EXECUTABLE is required")
endif()

# The child listens only on loopback and deliberately withholds its response.
# A crash, launch failure, connection failure or accidental PASS is insufficient.
execute_process(COMMAND "${TEST_EXECUTABLE}" --stall-response
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    TIMEOUT 10)
if(NOT "${result}" STREQUAL "1" OR
   NOT output MATCHES "LOOPBACK_REQUEST_RECEIVED" OR
   NOT output MATCHES "HTTP_WATCHDOG_EXPIRED" OR
   NOT error MATCHES "TEST_CHECK failed: !watchdogExpired")
    message(FATAL_ERROR "HTTP watchdog did not produce the expected test failure: ${result}\n${output}\n${error}")
endif()
message(STATUS "HTTP watchdog: received loopback request, withheld response, test failed as required")
