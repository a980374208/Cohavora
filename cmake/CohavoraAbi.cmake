include_guard(GLOBAL)

set(COHAVORA_DEBUG_DEPENDENCY_ABI "Release" CACHE STRING
    "Prebuilt dependency ABI used by the Debug configuration (Release or Debug)")
set_property(CACHE COHAVORA_DEBUG_DEPENDENCY_ABI PROPERTY STRINGS Release Debug)
if(NOT COHAVORA_DEBUG_DEPENDENCY_ABI MATCHES "^(Release|Debug)$")
    message(FATAL_ERROR
        "COHAVORA_DEBUG_DEPENDENCY_ABI must be Release or Debug, got "
        "'${COHAVORA_DEBUG_DEPENDENCY_ABI}'.")
endif()

# Keep the selected CRT/STL ABI consistent across project targets and imported
# dependencies. Debug dependencies use the normal MSVC debug ABI; the default
# Release selection preserves fast Debug builds while still emitting symbols.
add_library(cohavora_dependency_abi INTERFACE)
add_library(cohavora::dependency_abi ALIAS cohavora_dependency_abi)
target_compile_definitions(cohavora_dependency_abi INTERFACE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:_ITERATOR_DEBUG_LEVEL=0>)

if(MSVC)
    if(COHAVORA_DEBUG_DEPENDENCY_ABI STREQUAL "Debug")
        target_compile_definitions(cohavora_dependency_abi INTERFACE
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Debug>>:_ITERATOR_DEBUG_LEVEL=2>)
        set(CMAKE_MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING
            "MSVC runtime selected for the bundled dependencies" FORCE)
        unset(CMAKE_MAP_IMPORTED_CONFIG_DEBUG CACHE)
        unset(CMAKE_MAP_IMPORTED_CONFIG_DEBUG)
    else()
        target_compile_definitions(cohavora_dependency_abi INTERFACE
            $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Debug>>:NDEBUG>
            $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Debug>>:_ITERATOR_DEBUG_LEVEL=0>)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING
            "MSVC runtime selected for the bundled dependencies" FORCE)
        set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG "Release;None" CACHE STRING
            "Imported configuration used by Release-ABI Debug builds" FORCE)
    endif()
endif()

# Apply the ABI contract to targets declared below this include, including
# targets in child directories. Standalone test projects keep their own ABI.
link_libraries(cohavora_dependency_abi)

message(STATUS
    "[ABI] Debug configuration uses ${COHAVORA_DEBUG_DEPENDENCY_ABI} dependencies")
