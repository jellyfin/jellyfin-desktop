if(UNIX AND NOT APPLE)
    add_executable(mpris_projection_test
        mpris_projection_test.cpp
        ${CMAKE_SOURCE_DIR}/src/playback/sinks/mpris/mpris_projection.cpp
    )
    target_include_directories(mpris_projection_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/doctest/include
    )
    add_test(NAME mpris_projection_test COMMAND mpris_projection_test)
endif()
