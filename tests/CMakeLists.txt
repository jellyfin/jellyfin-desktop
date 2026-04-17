add_executable(jellyfin_api_test
    jellyfin_api_test.cpp
    ${CMAKE_SOURCE_DIR}/src/jellyfin_api.cpp
    ${CMAKE_SOURCE_DIR}/src/cjson/cJSON.c
)

target_include_directories(jellyfin_api_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/doctest
)

add_test(NAME jellyfin_api_test COMMAND jellyfin_api_test)
