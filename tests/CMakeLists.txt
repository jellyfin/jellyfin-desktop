add_executable(cli_test
    cli_test.cpp
    ${CMAKE_SOURCE_DIR}/src/cli.cpp
)

target_include_directories(cli_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest/include
    ${MPV_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/third_party/cef
)

target_link_libraries(cli_test PRIVATE ${MPV_LIBRARIES} fmt::fmt jfn_rust)

add_test(NAME cli_test COMMAND cli_test)

add_executable(mpv_color_test
    mpv_color_test.cpp
    ${CMAKE_SOURCE_DIR}/src/mpv/color.cpp
)

target_include_directories(mpv_color_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest/include
)

add_test(NAME mpv_color_test COMMAND mpv_color_test)

add_executable(cef_color_test
    cef_color_test.cpp
    ${CMAKE_SOURCE_DIR}/src/cef/color.cpp
)

target_include_directories(cef_color_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest/include
)

add_test(NAME cef_color_test COMMAND cef_color_test)

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
