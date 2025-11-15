add_library(mtcreceiver mtcreceiver.cpp)
set_target_properties(mtcreceiver PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Include directories
target_include_directories(mtcreceiver PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RTMIDI_INCLUDE_DIRS}
)

# Link RtMidi
if(RTMIDI_LIBRARIES)
    target_link_libraries(mtcreceiver ${RTMIDI_LIBRARIES})
endif()
