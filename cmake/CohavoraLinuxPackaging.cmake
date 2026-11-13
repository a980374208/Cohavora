# The Linux package uses system desktop/media libraries. Only the application
# and its own renderer modules belong in the payload; never copy a dev sysroot.
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "The Ubuntu package profile currently supports amd64 only")
endif()

set(cohavora_linux_payload "lib/cohavora")
install(TARGETS cohavora_app RUNTIME DESTINATION "${cohavora_linux_payload}")
set_target_properties(cohavora_app PROPERTIES
    INSTALL_RPATH "" INSTALL_RPATH_USE_LINK_PATH FALSE)
if(COHAVORA_WEBRTC_HAS_LINUX_VAAPI)
    set_property(TARGET cohavora_app PROPERTY INSTALL_RPATH "$ORIGIN")
    install(FILES "${COHAVORA_WEBRTC_LINUX_VAAPI_RUNTIME}"
        DESTINATION "${cohavora_linux_payload}")
    # Keep the delivered source/relink layout identical to the SDK: relink.py
    # resolves its corresponding source relative to this document root.
    set(cohavora_vaapi_documents "share/doc/cohavora/third-party/webrtc-vaapi")
    foreach(material source/vaapi relink/vaapi licenses/vaapi)
        if(NOT IS_DIRECTORY "${WEBRTC_ROOT}/${material}")
            message(FATAL_ERROR "Linux VAAPI package lacks corresponding material: ${material}")
        endif()
        get_filename_component(parent "${material}" DIRECTORY)
        install(DIRECTORY "${WEBRTC_ROOT}/${material}"
            DESTINATION "${cohavora_vaapi_documents}/${parent}")
    endforeach()
    foreach(metadata package.json checksums.sha256)
        if(NOT EXISTS "${WEBRTC_ROOT}/metadata/${metadata}")
            message(FATAL_ERROR "Linux VAAPI package lacks SDK metadata: ${metadata}")
        endif()
        install(FILES "${WEBRTC_ROOT}/metadata/${metadata}"
            DESTINATION "${cohavora_vaapi_documents}/metadata")
    endforeach()
    file(SHA256 "${WEBRTC_ROOT}/metadata/package.json" cohavora_vaapi_profile_sha256)
    get_filename_component(cohavora_vaapi_runtime_name
        "${COHAVORA_WEBRTC_LINUX_VAAPI_RUNTIME}" NAME)
    file(WRITE "${CMAKE_BINARY_DIR}/packaging/linux/vaapi-delivery.json"
        "{\n  \"schema\": 1,\n  \"sdk_profile_sha256\": \"${cohavora_vaapi_profile_sha256}\",\n  \"runtime_file\": \"${cohavora_vaapi_runtime_name}\",\n  \"hardware_validation\": \"NOT_RUN\"\n}\n")
    file(WRITE "${CMAKE_BINARY_DIR}/packaging/linux/vaapi-README.md"
        "# Isolated VAAPI runtime\n\nThe runtime in ../../../../../lib/cohavora/ is paired with the SDK profile in metadata/package.json. Its corresponding FFmpeg archive, exact patch and runtime sources are in source/vaapi/.\n\nLicenses are in licenses/vaapi/. Rebuild/relink instructions and the linkable objects are in relink/vaapi/README.md and relink/vaapi/relink.py. Keep this directory layout when copying those materials. Rebuilding does not validate GPU encoding or resource retirement.\n")
    install(FILES "${CMAKE_BINARY_DIR}/packaging/linux/vaapi-delivery.json"
        DESTINATION "${cohavora_vaapi_documents}" RENAME delivery.json)
    install(FILES "${CMAKE_BINARY_DIR}/packaging/linux/vaapi-README.md"
        DESTINATION "${cohavora_vaapi_documents}" RENAME README.md)
endif()
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
if(COHAVORA_WEBRTC_HAS_LINUX_VAAPI)
    # CPackDeb otherwise strips every unstripped ELF and adds a debug link,
    # including this prebuilt DSO whose original SHA is part of the SDK ABI.
    # Keep ordinary application/renderer debug splitting unchanged.
    find_program(cohavora_package_python NAMES python3 REQUIRED)
    file(SHA256 "${COHAVORA_WEBRTC_LINUX_VAAPI_RUNTIME}" cohavora_vaapi_runtime_sha256)
    set(cohavora_objcopy_guard "${PROJECT_SOURCE_DIR}/packaging/linux/objcopy_guard.py")
    file(SHA256 "${cohavora_objcopy_guard}" cohavora_objcopy_guard_sha256)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cohavora_objcopy_guard}")
    set(CPACK_OBJCOPY_EXECUTABLE
        "${cohavora_package_python};${cohavora_objcopy_guard};--objcopy;${CMAKE_OBJCOPY};--runtime-sha256;${cohavora_vaapi_runtime_sha256};--guard-sha256;${cohavora_objcopy_guard_sha256};--")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE)
    set(CPACK_BUILD_CONFIG "${CMAKE_BUILD_TYPE}")
endif()
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/CohavoraCPackOptions.cmake")
set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_CURRENT_LIST_DIR}/CohavoraLinuxPackagePreflight.cmake")
set(CPACK_COHAVORA_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
set(CPACK_COHAVORA_BINARY_DIR "${CMAKE_BINARY_DIR}")
include(CPack)
