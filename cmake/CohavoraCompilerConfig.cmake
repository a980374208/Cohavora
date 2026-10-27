include_guard(GLOBAL)

# Cohavora Compiler Configuration Module
# Aligned with OBS Studio (cmake/windows/compilerconfig.cmake)
# Manages platform- and toolchain-specific compiler flags.

if(MSVC)
    # Enable MSVC multi-processor compilation (/MP) across all translation units.
    # Matches OBS Studio's _obs_msvc_c_options and _obs_msvc_cpp_options.
    set(_cohavora_msvc_c_options /MP)
    set(_cohavora_msvc_cpp_options /MP)

    add_compile_options(
        "$<$<COMPILE_LANG_AND_ID:C,MSVC>:${_cohavora_msvc_c_options}>"
        "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:${_cohavora_msvc_cpp_options}>"
    )
endif()
