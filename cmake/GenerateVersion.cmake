# Generate version.h at build time
# Called via: cmake -P GenerateVersion.cmake
#
# APP_VERSION_FULL rules:
# - No git available (e.g. tarball build): APP_VERSION as-is.
# - HEAD clean and exactly tagged "v${APP_VERSION}" (release build): APP_VERSION as-is.
# - Otherwise: "${APP_VERSION}+${short-sha}", with "-dirty" appended when working tree has changes.
#
# Only rewrites version.h when its contents would actually change, to avoid
# triggering rebuilds of every TU that includes it.

set(VERSION_FILE "${BINARY_DIR}/src/version.h")

file(READ "${SOURCE_DIR}/VERSION" APP_VERSION)
string(STRIP "${APP_VERSION}" APP_VERSION)

file(READ "${SOURCE_DIR}/CEF_VERSION" APP_CEF_VERSION)
string(STRIP "${APP_CEF_VERSION}" APP_CEF_VERSION)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE APP_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT
)
if(NOT GIT_RESULT EQUAL 0 OR APP_GIT_SHA STREQUAL "")
    set(APP_VERSION_FULL "${APP_VERSION}")
else()
    execute_process(
        COMMAND git diff --quiet HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE DIRTY_RESULT
    )
    execute_process(
        COMMAND git describe --exact-match --tags HEAD
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE APP_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE TAG_RESULT
    )
    if(DIRTY_RESULT EQUAL 0 AND TAG_RESULT EQUAL 0 AND APP_GIT_TAG STREQUAL "v${APP_VERSION}")
        set(APP_VERSION_FULL "${APP_VERSION}")
    elseif(DIRTY_RESULT EQUAL 0)
        set(APP_VERSION_FULL "${APP_VERSION}+${APP_GIT_SHA}")
    else()
        set(APP_VERSION_FULL "${APP_VERSION}+${APP_GIT_SHA}-dirty")
    endif()
endif()

set(NEW_CONTENT
"#pragma once

#define APP_VERSION \"${APP_VERSION}\"
#define APP_VERSION_FULL \"${APP_VERSION_FULL}\"
#define APP_USER_AGENT \"JellyfinDesktop/${APP_VERSION_FULL}\"
#define APP_CEF_VERSION \"${APP_CEF_VERSION}\"
")

set(OLD_CONTENT "")
if(EXISTS "${VERSION_FILE}")
    file(READ "${VERSION_FILE}" OLD_CONTENT)
endif()

if(NOT "${NEW_CONTENT}" STREQUAL "${OLD_CONTENT}")
    file(WRITE "${VERSION_FILE}" "${NEW_CONTENT}")
    message(STATUS "version.h updated: ${APP_VERSION_FULL} cef=${APP_CEF_VERSION}")
endif()
