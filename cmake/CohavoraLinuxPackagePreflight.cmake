# Run after installation into CPack's staging tree, before generating any package.
# Refuse incomplete release metadata even when CPack was invoked directly.
set(payload "${CPACK_TEMPORARY_DIRECTORY}/usr")
foreach(required IN ITEMS
        lib/cohavora/Cohavora
        bin/cohavora
        share/applications/org.cohavora.Cohavora.desktop
        share/doc/cohavora/LICENSE
        share/doc/cohavora/build-info.json
        share/doc/cohavora/source-provenance.json)
    if(NOT EXISTS "${payload}/${required}")
        message(FATAL_ERROR "Linux package input missing: ${required}. Use packaging/linux/package.py.")
    endif()
endforeach()
find_program(cohavora_package_python NAMES python3 REQUIRED)
execute_process(COMMAND "${cohavora_package_python}"
    "${CPACK_COHAVORA_SOURCE_DIR}/packaging/linux/package.py"
    --build-dir "${CPACK_COHAVORA_BINARY_DIR}" --verify-inputs
    RESULT_VARIABLE identity_result)
if(NOT identity_result EQUAL 0)
    message(FATAL_ERROR "Linux package provenance does not match the source/build inputs")
endif()
foreach(dependency IN ITEMS qt webrtc vcpkg)
    file(GLOB_RECURSE notices "${payload}/share/doc/cohavora/third-party/${dependency}/*")
    if(NOT notices)
        message(FATAL_ERROR "Linux package requires the ${dependency} license notices")
    endif()
endforeach()
