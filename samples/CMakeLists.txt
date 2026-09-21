add_executable(livekit_media_broadcaster
    ${PROJECT_SOURCE_DIR}/src/media/livekit_media_broadcaster.cpp)
target_link_libraries(livekit_media_broadcaster PRIVATE livekit_signaling)

add_executable(simple_room
    ${PROJECT_SOURCE_DIR}/src/app/simple_room.cpp)
target_link_libraries(simple_room PRIVATE livekit_signaling)

add_executable(simple_rpc
    ${PROJECT_SOURCE_DIR}/src/app/simple_rpc.cpp)
target_link_libraries(simple_rpc PRIVATE livekit_signaling)

foreach(livekit_sample_target IN ITEMS
        livekit_media_broadcaster
        simple_room
        simple_rpc)
    set_property(TARGET ${livekit_sample_target} PROPERTY FOLDER "samples")
endforeach()
