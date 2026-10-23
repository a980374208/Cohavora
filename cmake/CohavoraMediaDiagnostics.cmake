include_guard(GLOBAL)

option(COHAVORA_ENABLE_MEDIA_DIAGNOSTICS
    "Compile callback/snapshot and E2EE frame instrumentation (use a separate diagnostic build tree)" OFF)
add_library(cohavora_media_diagnostics INTERFACE)
target_compile_definitions(cohavora_media_diagnostics INTERFACE
    COHAVORA_ENABLE_MEDIA_DIAGNOSTICS=$<BOOL:${COHAVORA_ENABLE_MEDIA_DIAGNOSTICS}>)
set_property(TARGET cohavora_media_diagnostics PROPERTY
    INTERFACE_COHAVORA_MEDIA_DIAGNOSTICS ${COHAVORA_ENABLE_MEDIA_DIAGNOSTICS})
set_property(TARGET cohavora_media_diagnostics APPEND PROPERTY
    COMPATIBLE_INTERFACE_BOOL COHAVORA_MEDIA_DIAGNOSTICS)
# Also cover independently compiled product sources in child test/UI targets.
link_libraries(cohavora_media_diagnostics)
