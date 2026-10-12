# Configure these when packaging the client. The signing private key stays with
# the publisher; only the raw Ed25519 public key is embedded in the application.
set(COHAVORA_UPDATE_PRIMARY_URL
    "http://123.56.225.164/downloads/latest.json" CACHE STRING
    "Signed stable manifest on the Alibaba Cloud Web server")
set(COHAVORA_UPDATE_FALLBACK_URL
    "https://github.com/a980374208/Cohavora/releases/latest/download/latest.json"
    CACHE STRING "Public HTTPS URL of the identical signed fallback manifest")
set(COHAVORA_UPDATE_PUBLIC_KEY_HEX "" CACHE STRING
    "Trusted raw Ed25519 public key (64 hexadecimal characters)")

if(COHAVORA_UPDATE_PUBLIC_KEY_HEX)
    string(LENGTH "${COHAVORA_UPDATE_PUBLIC_KEY_HEX}" key_length)
    if(NOT key_length EQUAL 64 OR
            NOT COHAVORA_UPDATE_PUBLIC_KEY_HEX MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "COHAVORA_UPDATE_PUBLIC_KEY_HEX must contain 64 hex characters")
    endif()
endif()
if(COHAVORA_UPDATE_PRIMARY_URL
        AND NOT COHAVORA_UPDATE_PRIMARY_URL MATCHES "^https://"
        AND NOT COHAVORA_UPDATE_PRIMARY_URL MATCHES
            "^http://123\\.56\\.225\\.164(:80)?/downloads/.+$")
    message(FATAL_ERROR "COHAVORA_UPDATE_PRIMARY_URL must use HTTPS or the configured Alibaba Cloud downloads origin")
endif()
if(COHAVORA_UPDATE_FALLBACK_URL AND NOT COHAVORA_UPDATE_FALLBACK_URL MATCHES "^https://")
    message(FATAL_ERROR "COHAVORA_UPDATE_FALLBACK_URL must be an HTTPS URL")
endif()
if(NOT COHAVORA_UPDATE_PRIMARY_URL OR NOT COHAVORA_UPDATE_PUBLIC_KEY_HEX)
    message(STATUS "[Updates] Web manifest URL/public key not configured; update checks remain disabled")
endif()

# Escape user-supplied cache strings before inserting them in C++ literals.
foreach(update_option IN ITEMS
        COHAVORA_UPDATE_PRIMARY_URL COHAVORA_UPDATE_FALLBACK_URL COHAVORA_UPDATE_PUBLIC_KEY_HEX)
    set(${update_option}_CPP "${${update_option}}")
    string(REPLACE "\\" "\\\\" ${update_option}_CPP "${${update_option}_CPP}")
    string(REPLACE "\"" "\\\"" ${update_option}_CPP "${${update_option}_CPP}")
    string(REPLACE "\n" "\\n" ${update_option}_CPP "${${update_option}_CPP}")
    string(REPLACE "\r" "\\r" ${update_option}_CPP "${${update_option}_CPP}")
endforeach()
configure_file(${PROJECT_SOURCE_DIR}/src/update/update_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/src/update/update_config.h @ONLY)

add_library(cohavora_updates STATIC
    ${PROJECT_SOURCE_DIR}/src/update/update_manifest.cpp
    ${PROJECT_SOURCE_DIR}/src/update/update_manifest.h
    ${PROJECT_SOURCE_DIR}/src/update/update_service.cpp
    ${PROJECT_SOURCE_DIR}/src/update/update_service.h)
target_include_directories(cohavora_updates PUBLIC ${PROJECT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(cohavora_updates PUBLIC cohavora::qt_network_runtime
    PRIVATE OpenSSL::SSL OpenSSL::Crypto)
if(WIN32)
    target_link_libraries(cohavora_updates PRIVATE advapi32 shell32)
endif()
set_target_properties(cohavora_updates PROPERTIES AUTOMOC ON FOLDER "app/updates")
if(MSVC)
    target_compile_options(cohavora_updates PRIVATE /utf-8)
endif()
