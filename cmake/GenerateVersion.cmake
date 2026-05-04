# Generate version.h at build time
# Called via: cmake -P GenerateVersion.cmake

set(VERSION_FILE "${BINARY_DIR}/src/version.h")
set(VERSION_CACHE "${BINARY_DIR}/src/.version_cache")
set(HASH_CACHE "${BINARY_DIR}/src/.git_hash_cache")
set(EPOCH_CACHE "${BINARY_DIR}/src/.epoch_cache")
set(CEF_CACHE "${BINARY_DIR}/src/.cef_version_cache")

# Read current values
file(READ "${SOURCE_DIR}/VERSION" APP_VERSION)
string(STRIP "${APP_VERSION}" APP_VERSION)

file(READ "${SOURCE_DIR}/CEF_VERSION" APP_CEF_VERSION)
string(STRIP "${APP_CEF_VERSION}" APP_CEF_VERSION)

execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE APP_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT
)
if(NOT GIT_RESULT EQUAL 0 OR APP_GIT_HASH STREQUAL "")
    set(APP_GIT_HASH "")
    set(HAS_GIT_HASH 0)
else()
    set(HAS_GIT_HASH 1)
endif()

set(SOURCE_EPOCH "$ENV{SOURCE_DATE_EPOCH}")

# Read cached values
set(CACHED_VERSION "")
set(CACHED_HASH "")
set(CACHED_EPOCH "")
set(CACHED_CEF "")
if(EXISTS "${VERSION_CACHE}")
    file(READ "${VERSION_CACHE}" CACHED_VERSION)
endif()
if(EXISTS "${HASH_CACHE}")
    file(READ "${HASH_CACHE}" CACHED_HASH)
endif()
if(EXISTS "${EPOCH_CACHE}")
    file(READ "${EPOCH_CACHE}" CACHED_EPOCH)
endif()
if(EXISTS "${CEF_CACHE}")
    file(READ "${CEF_CACHE}" CACHED_CEF)
endif()

# Update if changed
if(NOT "${APP_VERSION}" STREQUAL "${CACHED_VERSION}" OR
   NOT "${APP_GIT_HASH}" STREQUAL "${CACHED_HASH}" OR
   NOT "${SOURCE_EPOCH}" STREQUAL "${CACHED_EPOCH}" OR
   NOT "${APP_CEF_VERSION}" STREQUAL "${CACHED_CEF}")
    file(WRITE "${VERSION_CACHE}" "${APP_VERSION}")
    file(WRITE "${HASH_CACHE}" "${APP_GIT_HASH}")
    file(WRITE "${EPOCH_CACHE}" "${SOURCE_EPOCH}")
    file(WRITE "${CEF_CACHE}" "${APP_CEF_VERSION}")
    if(HAS_GIT_HASH)
        set(APP_VERSION_STRING "${APP_VERSION}+${APP_GIT_HASH}")
    else()
        set(APP_VERSION_STRING "${APP_VERSION}")
    endif()
    file(WRITE "${VERSION_FILE}"
"#pragma once

#define APP_VERSION \"${APP_VERSION}\"
#define APP_VERSION_STRING \"${APP_VERSION_STRING}\"
#define APP_USER_AGENT \"JellyfinDesktop/${APP_VERSION_STRING}\"
#define APP_CEF_VERSION \"${APP_CEF_VERSION}\"
")
    message(STATUS "version.h updated: ${APP_VERSION}+${APP_GIT_HASH} cef=${APP_CEF_VERSION}")
endif()
