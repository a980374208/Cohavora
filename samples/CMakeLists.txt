if(WIN32)
    # This sample uses WASAPI, DirectShow and COM capture APIs.
    add_executable(cohavora_media_broadcaster
        livekit_media_broadcaster.cpp)
    target_link_libraries(cohavora_media_broadcaster PRIVATE cohavora_core)
    set_property(TARGET cohavora_media_broadcaster PROPERTY FOLDER "samples")
endif()

add_executable(simple_room
    simple_room.cpp)
target_link_libraries(simple_room PRIVATE cohavora_core)

add_executable(simple_rpc
    simple_rpc.cpp)
target_link_libraries(simple_rpc PRIVATE cohavora_core)

foreach(livekit_sample_target IN ITEMS
        simple_room
        simple_rpc)
    set_property(TARGET ${livekit_sample_target} PROPERTY FOLDER "samples")
endforeach()
