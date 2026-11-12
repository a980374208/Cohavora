# The Linux package uses system desktop/media libraries. Only the application
# and its own renderer modules belong in the payload; never copy a dev sysroot.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "The Ubuntu package profile currently supports amd64 only")
endif()

set(cohavora_linux_payload "lib/cohavora")
install(TARGETS cohavora_app RUNTIME DESTINATION "${cohavora_linux_payload}")
set_target_properties(cohavora_app PROPERTIES
    INSTALL_RPATH "" INSTALL_RPATH_USE_LINK_PATH FALSE)
foreach(backend IN ITEMS dx11 opengl)
    if(TARGET cohavora_render_${backend})
        install(TARGETS cohavora_render_${backend}
            LIBRARY DESTINATION "${cohavora_linux_payload}/renderers")
        set_target_properties(cohavora_render_${backend} PROPERTIES
            INSTALL_RPATH "" INSTALL_RPATH_USE_LINK_PATH FALSE)
    endif()
endforeach()
install(PROGRAMS "${PROJECT_SOURCE_DIR}/packaging/linux/cohavora"
    DESTINATION bin)
install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/org.cohavora.Cohavora.desktop"
    DESTINATION share/applications)
install(FILES "${PROJECT_SOURCE_DIR}/src/ui/icons/cohavora.svg"
    DESTINATION share/icons/hicolor/scalable/apps RENAME org.cohavora.Cohavora.svg)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
    DESTINATION share/doc/cohavora)
install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/README.md"
    DESTINATION share/doc/cohavora RENAME linux-package.md)
if(IS_DIRECTORY "${WEBRTC_ROOT}/licenses")
    install(DIRECTORY "${WEBRTC_ROOT}/licenses/"
        DESTINATION share/doc/cohavora/third-party/webrtc)
endif()
if(IS_DIRECTORY "${QT_ROOT}/licenses")
    install(DIRECTORY "${QT_ROOT}/licenses/"
        DESTINATION share/doc/cohavora/third-party/qt)
endif()
# package.py collects the matching Qt source notices without changing the SDK.
# This generated directory is packaging input, never an alternative SDK prefix.
install(DIRECTORY "${CMAKE_BINARY_DIR}/packaging/linux/third-party/qt/"
    DESTINATION share/doc/cohavora/third-party/qt OPTIONAL)
install(FILES "${CMAKE_BINARY_DIR}/packaging/linux/source-provenance.json"
    DESTINATION share/doc/cohavora OPTIONAL)
if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    file(GLOB cohavora_port_copyrights
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
    foreach(copyright IN LISTS cohavora_port_copyrights)
        get_filename_component(port_directory "${copyright}" DIRECTORY)
        get_filename_component(port "${port_directory}" NAME)
        install(FILES "${copyright}"
            DESTINATION "share/doc/cohavora/third-party/vcpkg/${port}")
    endforeach()
endif()

execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE cohavora_linux_source_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
string(TIMESTAMP cohavora_linux_package_configured_utc "%Y-%m-%dT%H:%M:%SZ" UTC)
set(cohavora_linux_package_audio false)
if(COHAVORA_LINUX_SYSTEM_AUDIO_VERIFIED)
    set(cohavora_linux_package_audio true)
endif()
# A packaging correction can advance the Debian revision without inventing a
# new application version. Keep payload metadata and the DEB control aligned.
set(CPACK_DEBIAN_PACKAGE_RELEASE "1" CACHE STRING "Cohavora Debian package revision")
if(NOT CPACK_DEBIAN_PACKAGE_RELEASE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "CPACK_DEBIAN_PACKAGE_RELEASE must be a positive integer")
endif()
configure_file("${PROJECT_SOURCE_DIR}/packaging/linux/build-info.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/packaging/linux/build-info.json" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/packaging/linux/build-info.json"
    DESTINATION share/doc/cohavora)

set(CPACK_PACKAGE_NAME "Cohavora")
set(CPACK_PACKAGE_VENDOR "Cohavora")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Cohavora desktop meeting client")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_SYSTEM_NAME "Ubuntu24.04-amd64")
set(CPACK_PACKAGE_FILE_NAME "Cohavora-${PROJECT_VERSION}-${CPACK_SYSTEM_NAME}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_GENERATOR "DEB;TGZ")
set(CPACK_DEBIAN_PACKAGE_NAME "cohavora")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Cohavora contributors")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# PipeWire is loaded by SONAME rather than linked. A Secret Service daemon is
# optional: credentials stay in memory when no supported keyring is available.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libpipewire-0.3-0t64 (>= 1.0), xdg-desktop-portal")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "xdg-desktop-portal-gnome, gnome-keyring")
set(CPACK_DEBIAN_DEBUGINFO_PACKAGE ON)
if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE)
    set(CPACK_BUILD_CONFIG "${CMAKE_BUILD_TYPE}")
endif()
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/CohavoraCPackOptions.cmake")
set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_CURRENT_LIST_DIR}/CohavoraLinuxPackagePreflight.cmake")
set(CPACK_COHAVORA_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
set(CPACK_COHAVORA_BINARY_DIR "${CMAKE_BINARY_DIR}")
include(CPack)
