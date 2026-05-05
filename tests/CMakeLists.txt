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

add_executable(log_redact_test
    log_redact_test.cpp
    ${CMAKE_SOURCE_DIR}/src/log_redact.cpp
)

target_include_directories(log_redact_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest/include
)

add_test(NAME log_redact_test COMMAND log_redact_test)
