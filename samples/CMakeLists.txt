add_executable(cohavora_media_broadcaster
    ${PROJECT_SOURCE_DIR}/src/media/livekit_media_broadcaster.cpp)
target_link_libraries(cohavora_media_broadcaster PRIVATE cohavora_core)

add_executable(simple_room
    ${PROJECT_SOURCE_DIR}/src/app/simple_room.cpp)
target_link_libraries(simple_room PRIVATE cohavora_core)

add_executable(simple_rpc
    ${PROJECT_SOURCE_DIR}/src/app/simple_rpc.cpp)
target_link_libraries(simple_rpc PRIVATE cohavora_core)

foreach(livekit_sample_target IN ITEMS
        cohavora_media_broadcaster
        simple_room
        simple_rpc)
    set_property(TARGET ${livekit_sample_target} PROPERTY FOLDER "samples")
endforeach()
