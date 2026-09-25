include_guard(GLOBAL)

# The bundled WebRTC and Qt SDKs are Release-ABI static libraries. Keep the
# required ABI contract in one target so every project target receives the
# same definitions, including targets created in subdirectories.
add_library(cohavora_release_abi INTERFACE)
add_library(cohavora::release_abi ALIAS cohavora_release_abi)
target_compile_definitions(cohavora_release_abi INTERFACE
    $<$<COMPILE_LANGUAGE:C,CXX>:NDEBUG>
    $<$<COMPILE_LANGUAGE:CXX>:_ITERATOR_DEBUG_LEVEL=0>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Debug>>:_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Debug>>:_ALLOW_RUNTIME_LIBRARY_MISMATCH>)
target_link_options(cohavora_release_abi INTERFACE
    $<$<AND:$<LINK_LANG_AND_ID:CXX,MSVC>,$<CONFIG:Debug>>:/NODEFAULTLIB:libcpmtd.lib>
    $<$<AND:$<LINK_LANG_AND_ID:CXX,MSVC>,$<CONFIG:Debug>>:/NODEFAULTLIB:libcmtd.lib>)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING
        "MSVC runtime required by the bundled Release-ABI dependencies" FORCE)
endif()

# Debug project targets intentionally consume the Release-only Qt SDK.
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG "Release;None")

# Apply the ABI contract to targets declared below this include, including
# targets in child directories. Standalone test projects keep their own ABI.
link_libraries(cohavora_release_abi)
