# FindCEF.cmake - Find CEF binary distribution

# Auto-detect external CEF from /opt if it exists
set(_DEFAULT_EXTERNAL_CEF_DIR "")
if(EXISTS "/opt/jellyfin-desktop/libcef/include/cef_version.h")
    set(_DEFAULT_EXTERNAL_CEF_DIR "/opt/jellyfin-desktop/libcef")
endif()

set(EXTERNAL_CEF_DIR "${_DEFAULT_EXTERNAL_CEF_DIR}" CACHE PATH "Path to external CEF installation (with prebuilt libcef_dll_wrapper.a)")

if(EXTERNAL_CEF_DIR)
    # Use external CEF installation
    message(STATUS "Using external CEF from: ${EXTERNAL_CEF_DIR}")

    if(NOT EXISTS "${EXTERNAL_CEF_DIR}/include/cef_version.h")
        message(FATAL_ERROR "CEF not found at ${EXTERNAL_CEF_DIR}. Missing include/cef_version.h")
    endif()

    # Read CEF version
    file(READ "${EXTERNAL_CEF_DIR}/include/cef_version.h" CEF_VERSION_CONTENT)
    string(REGEX MATCH "CEF_VERSION \"([^\"]+)\"" _ ${CEF_VERSION_CONTENT})
    set(CEF_VERSION ${CMAKE_MATCH_1})
    message(STATUS "Found CEF: ${CEF_VERSION}")

    set(CEF_INCLUDE_DIRS "${EXTERNAL_CEF_DIR}" "${EXTERNAL_CEF_DIR}/include")
    # External CEF: include/ for headers, lib/ for libraries and resources
    set(CEF_RESOURCE_DIR "${EXTERNAL_CEF_DIR}/lib")
    set(CEF_RELEASE_DIR "${EXTERNAL_CEF_DIR}/lib")

    # Find wrapper library
    if(WIN32)
        set(_WRAPPER_SEARCH_PATHS
            "${EXTERNAL_CEF_DIR}/lib/libcef_dll_wrapper.lib"
            "${EXTERNAL_CEF_DIR}/libcef_dll_wrapper/libcef_dll_wrapper.lib"
        )
    else()
        set(_WRAPPER_SEARCH_PATHS
            "${EXTERNAL_CEF_DIR}/lib/libcef_dll_wrapper.a"
        )
    endif()

    set(CEF_WRAPPER_PATH "")
    foreach(_path ${_WRAPPER_SEARCH_PATHS})
        if(EXISTS "${_path}")
            set(CEF_WRAPPER_PATH "${_path}")
            break()
        endif()
    endforeach()

    if(NOT CEF_WRAPPER_PATH)
        message(FATAL_ERROR "libcef_dll_wrapper not found. Searched: ${_WRAPPER_SEARCH_PATHS}")
    endif()

    message(STATUS "Using libcef_dll_wrapper: ${CEF_WRAPPER_PATH}")

    if(WIN32)
        set(CEF_LIBRARIES
            "${EXTERNAL_CEF_DIR}/lib/libcef.lib"
            "${CEF_WRAPPER_PATH}"
        )
    elseif(APPLE)
        set(CEF_LIBRARIES
            "${EXTERNAL_CEF_DIR}/lib/Chromium Embedded Framework.framework"
            "${CEF_WRAPPER_PATH}"
        )
    else() # Linux
        set(CEF_LIBRARIES
            "${EXTERNAL_CEF_DIR}/lib/libcef.so"
            "${CEF_WRAPPER_PATH}"
        )
    endif()

else()
    # Build CEF wrapper from CEF_ROOT
    if(NOT CEF_ROOT)
        message(FATAL_ERROR "CEF_ROOT not set. Download CEF from https://cef-builds.spotifycdn.com/index.html and extract to third_party/cef/")
    endif()

    if(NOT EXISTS "${CEF_ROOT}/include/cef_version.h")
        message(FATAL_ERROR "CEF not found at ${CEF_ROOT}. Ensure CEF binary distribution is extracted there.")
    endif()

    # Read CEF version
    file(READ "${CEF_ROOT}/include/cef_version.h" CEF_VERSION_CONTENT)
    string(REGEX MATCH "CEF_VERSION \"([^\"]+)\"" _ ${CEF_VERSION_CONTENT})
    set(CEF_VERSION ${CMAKE_MATCH_1})
    message(STATUS "Found CEF: ${CEF_VERSION}")

    set(CEF_INCLUDE_DIRS "${CEF_ROOT}" "${CEF_ROOT}/include")
    set(CEF_RESOURCE_DIR "${CEF_ROOT}/Resources")
    set(CEF_RELEASE_DIR "${CEF_ROOT}/Release")

    # Find libcef_dll_wrapper - check multiple possible locations
    if(WIN32)
        # Windows: Ninja builds to build/libcef_dll_wrapper/, MSBuild to build/libcef_dll_wrapper/Release/
        set(_WRAPPER_SEARCH_PATHS
            "${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.lib"
            "${CEF_ROOT}/build/libcef_dll_wrapper/Release/libcef_dll_wrapper.lib"
        )
        set(_WRAPPER_EXT ".lib")
    elseif(APPLE)
        set(_WRAPPER_SEARCH_PATHS
            "${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.a"
            "${CEF_ROOT}/build/libcef_dll_wrapper/Release/libcef_dll_wrapper.a"
        )
        set(_WRAPPER_EXT ".a")
    else() # Linux
        set(_WRAPPER_SEARCH_PATHS
            "${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.a"
        )
        set(_WRAPPER_EXT ".a")
    endif()

    # Find wrapper in search paths
    set(CEF_WRAPPER_PATH "")
    foreach(_path ${_WRAPPER_SEARCH_PATHS})
        if(EXISTS "${_path}")
            set(CEF_WRAPPER_PATH "${_path}")
            break()
        endif()
    endforeach()

    # Build wrapper if not found
    if(NOT CEF_WRAPPER_PATH)
        message(STATUS "libcef_dll_wrapper not found, building...")
        # Pass generator and architecture settings to match the main build
        set(_CEF_CMAKE_ARGS -B build -DCMAKE_BUILD_TYPE=Release)
        if(CMAKE_GENERATOR)
            list(APPEND _CEF_CMAKE_ARGS -G "${CMAKE_GENERATOR}")
        endif()
        if(WIN32 AND CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64")
            list(APPEND _CEF_CMAKE_ARGS -DPROJECT_ARCH=arm64)
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} ${_CEF_CMAKE_ARGS}
            WORKING_DIRECTORY ${CEF_ROOT}
            RESULT_VARIABLE CEF_CONFIG_RESULT
        )
        if(NOT CEF_CONFIG_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to configure CEF")
        endif()
        # Limit parallelism - CEF wrapper builds OOM with unlimited -j even on 16GB RAM
        # Windows multi-config generators need --config Release
        if(WIN32)
            set(_CEF_BUILD_CMD ${CMAKE_COMMAND} --build build --target libcef_dll_wrapper --config Release -j2)
        else()
            set(_CEF_BUILD_CMD ${CMAKE_COMMAND} --build build --target libcef_dll_wrapper -j2)
        endif()
        execute_process(
            COMMAND ${_CEF_BUILD_CMD}
            WORKING_DIRECTORY ${CEF_ROOT}
            RESULT_VARIABLE CEF_BUILD_RESULT
        )
        if(NOT CEF_BUILD_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to build libcef_dll_wrapper")
        endif()
        # Search again after build
        foreach(_path ${_WRAPPER_SEARCH_PATHS})
            if(EXISTS "${_path}")
                set(CEF_WRAPPER_PATH "${_path}")
                break()
            endif()
        endforeach()
        if(NOT CEF_WRAPPER_PATH)
            message(FATAL_ERROR "libcef_dll_wrapper not found after build. Searched: ${_WRAPPER_SEARCH_PATHS}")
        endif()
    endif()

    message(STATUS "Using libcef_dll_wrapper: ${CEF_WRAPPER_PATH}")

    # Platform-specific library setup
    if(WIN32)
        set(CEF_LIBRARIES
            "${CEF_RELEASE_DIR}/libcef.lib"
            "${CEF_WRAPPER_PATH}"
        )
    elseif(APPLE)
        set(CEF_LIBRARIES
            "${CEF_RELEASE_DIR}/Chromium Embedded Framework.framework"
            "${CEF_WRAPPER_PATH}"
        )
    else() # Linux
        set(CEF_LIBRARIES
            "${CEF_RELEASE_DIR}/libcef.so"
            "${CEF_WRAPPER_PATH}"
        )
    endif()
endif()

set(CEF_FOUND TRUE)
