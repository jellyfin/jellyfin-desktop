add_executable(jellyfin_api_test
    jellyfin_api_test.cpp
    ${CMAKE_SOURCE_DIR}/src/jellyfin/api.cpp
    ${CMAKE_SOURCE_DIR}/src/cjson/cJSON.c
)

target_include_directories(jellyfin_api_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest/include
)

add_test(NAME jellyfin_api_test COMMAND jellyfin_api_test)

set(_settings_test_paths
    ${CMAKE_SOURCE_DIR}/src/paths/paths.cpp
)
if(APPLE)
    list(APPEND _settings_test_paths ${CMAKE_SOURCE_DIR}/src/paths/macos.cpp)
elseif(WIN32)
    list(APPEND _settings_test_paths ${CMAKE_SOURCE_DIR}/src/paths/windows.cpp)
else()
    list(APPEND _settings_test_paths ${CMAKE_SOURCE_DIR}/src/paths/linux.cpp)
endif()

add_executable(settings_test
    settings_test.cpp
    ${CMAKE_SOURCE_DIR}/src/settings.cpp
    ${CMAKE_SOURCE_DIR}/src/cjson/cJSON.c
    ${_settings_test_paths}
)

target_include_directories(settings_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest/include
)

add_test(NAME settings_test COMMAND settings_test)

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

target_link_libraries(cli_test PRIVATE ${MPV_LIBRARIES} quill::quill)

add_test(NAME cli_test COMMAND cli_test)

add_executable(log_redact_test
    log_redact_test.cpp
    ${CMAKE_SOURCE_DIR}/src/log_redact.cpp
)

target_include_directories(log_redact_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest/include
)

add_test(NAME log_redact_test COMMAND log_redact_test)

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

add_executable(playback_state_machine_test
    playback_state_machine_test.cpp
    ${CMAKE_SOURCE_DIR}/src/playback/state_machine.cpp
)
target_include_directories(playback_state_machine_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest/include
)
add_test(NAME playback_state_machine_test COMMAND playback_state_machine_test)

set(_wake_event_src ${CMAKE_SOURCE_DIR}/src/wake_event_linux.cpp)
if(APPLE)
    set(_wake_event_src ${CMAKE_SOURCE_DIR}/src/wake_event_macos.cpp)
elseif(WIN32)
    set(_wake_event_src ${CMAKE_SOURCE_DIR}/src/wake_event_windows.cpp)
endif()

add_executable(playback_coordinator_test
    playback_coordinator_test.cpp
    ${CMAKE_SOURCE_DIR}/src/playback/state_machine.cpp
    ${CMAKE_SOURCE_DIR}/src/playback/coordinator.cpp
    ${_wake_event_src}
)
target_include_directories(playback_coordinator_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest/include
)
if(NOT WIN32)
    target_link_libraries(playback_coordinator_test PRIVATE pthread)
endif()
add_test(NAME playback_coordinator_test COMMAND playback_coordinator_test)

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
