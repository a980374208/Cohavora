if(COHAVORA_BUILD_QT_TESTS)
    add_library(cohavora_qt_test_environment INTERFACE)
    target_include_directories(cohavora_qt_test_environment BEFORE INTERFACE
        ${LIVEKIT_PROJECT_SOURCE_DIR}
        ${GEN_DIR}
        ${WEBRTC_ROOT}/include/third_party/libyuv/include)
    target_link_libraries(cohavora_qt_test_environment INTERFACE
        cohavora_dual_tls_link_compat
        cohavora::desktop_ui_runtime
        cohavora_core)
    target_compile_definitions(cohavora_qt_test_environment INTERFACE
        WIN32
        _WINDOWS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WINSOCK_DEPRECATED_NO_WARNINGS
        CRL_USE_WINAPI
        ASIO_STANDALONE
        WEBRTC_WIN)

    function(livekit_configure_qt_test target)
        target_link_libraries(${target} PRIVATE cohavora_qt_test_environment)
        set_target_properties(${target} PROPERTIES AUTOMOC ON)
    endfunction()
endif()

function(livekit_apply_default_test_timeout default_timeout)
    get_property(registered_tests DIRECTORY PROPERTY TESTS)
    foreach(registered_test IN LISTS registered_tests)
        get_property(timeout_is_set TEST "${registered_test}" PROPERTY TIMEOUT SET)
        if(NOT timeout_is_set)
            set_tests_properties("${registered_test}" PROPERTIES
                TIMEOUT "${default_timeout}")
        endif()
    endforeach()
endfunction()
