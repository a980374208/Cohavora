# Qt, the CRT and the application dependencies are linked statically. Render
# backends are loaded relative to the executable and must keep this layout.
install(TARGETS cohavora_app RUNTIME DESTINATION .)
foreach(backend IN ITEMS dx11 opengl)
    if(TARGET cohavora_render_${backend})
        install(TARGETS cohavora_render_${backend}
            RUNTIME DESTINATION renderers
            LIBRARY DESTINATION renderers)
    endif()
endforeach()

set(CPACK_PACKAGE_NAME "Cohavora")
set(CPACK_PACKAGE_VENDOR "Cohavora")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${COHAVORA_DISPLAY_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Cohavora")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "Cohavora")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$"
        OR CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64")
    set(cohavora_package_arch "arm64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(cohavora_package_arch "x64")
else()
    set(cohavora_package_arch "x86")
endif()
set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${cohavora_package_arch}")
set(CPACK_PACKAGE_FILE_NAME "Cohavora-${PROJECT_VERSION}-${CPACK_SYSTEM_NAME}")

# ZIP works with CMake alone. On Windows, cpack -G NSIS produces an installer
# with the same product identity when NSIS 3 is installed.
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_EXECUTABLES "Cohavora;${COHAVORA_DISPLAY_NAME}")
set(CPACK_CREATE_DESKTOP_LINKS "Cohavora")
set(CPACK_NSIS_DISPLAY_NAME "${COHAVORA_DISPLAY_NAME}")
set(CPACK_NSIS_PACKAGE_NAME "Cohavora")
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_NSIS_MUI_ICON "${PROJECT_SOURCE_DIR}/src/ui/icons/cohavora.ico")
set(CPACK_NSIS_MUI_UNIICON "${PROJECT_SOURCE_DIR}/src/ui/icons/cohavora.ico")
set(CPACK_NSIS_INSTALLED_ICON_NAME "Cohavora.exe")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall Cohavora")
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

# Make the single-config selection available to the per-generator CPack hook.
if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE)
    set(CPACK_BUILD_CONFIG "${CMAKE_BUILD_TYPE}")
endif()
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/CohavoraCPackOptions.cmake")
include(CPack)
