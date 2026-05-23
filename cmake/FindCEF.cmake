# FindCEF.cmake - Find CEF binary distribution

# Auto-detect external CEF from /opt if it exists
set(_DEFAULT_EXTERNAL_CEF_DIR "")
if(EXISTS "/opt/jellyfin-desktop/libcef/include/cef_version.h")
    set(_DEFAULT_EXTERNAL_CEF_DIR "/opt/jellyfin-desktop/libcef")
endif()

set(EXTERNAL_CEF_DIR "${_DEFAULT_EXTERNAL_CEF_DIR}" CACHE PATH "Path to external CEF installation (with prebuilt libcef_dll_wrapper.a)")

# Auto-detect system CEF from the "cef" package (used as last resort).
# Skip when EXTERNAL_CEF_DIR or CEF_ROOT already point at a real SDK, since
# those branches win in the elseif chain below; defaulting USE_SYSTEM_CEF=ON
# anyway leaves the cache flag lying about which CEF is actually selected,
# which then mis-gates resource-copy and compile-define logic in the parent
# CMakeLists.txt.
set(_DEFAULT_USE_SYSTEM_CEF OFF)
if(EXISTS "/usr/include/cef/include/cef_version.h"
   AND NOT EXTERNAL_CEF_DIR
   AND NOT (CEF_ROOT AND EXISTS "${CEF_ROOT}/include/cef_version.h"))
    set(_DEFAULT_USE_SYSTEM_CEF ON)
endif()

option(USE_SYSTEM_CEF "Use system-installed CEF from the 'cef' package" ${_DEFAULT_USE_SYSTEM_CEF})

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

    # A pre-built wrapper here must have been compiled with -Dapi_version
    # matching cef-rs's binding ABI. If the packager didn't pin it the wrapper
    # defaults to CEF_API_VERSION_EXPERIMENTAL (999999) and ctocpp wrap
    # runtime-aborts on the first Rust→C++ handoff. The packager-side
    # CMakeCache.txt — if shipped alongside the wrapper — is the only on-disk
    # record we can verify against; without it we cannot prove the pin held,
    # so fail loud rather than silently shipping a mis-pinned wrapper.
    set(_CEF_EXT_CACHE "${EXTERNAL_CEF_DIR}/build/CMakeCache.txt")
    if(NOT EXISTS "${_CEF_EXT_CACHE}")
        message(FATAL_ERROR
            "Pre-built libcef_dll_wrapper at ${CEF_WRAPPER_PATH} but no "
            "CMakeCache.txt at ${_CEF_EXT_CACHE} to verify it was built with "
            "api_version=${JFN_CEF_API_VERSION}. Either ship that cache file "
            "next to the wrapper, drop the pre-built wrapper and point "
            "CEF_ROOT at the SDK source so we build it here, or rebuild the "
            "wrapper out-of-tree with -Dapi_version=${JFN_CEF_API_VERSION}.")
    endif()
    file(READ "${_CEF_EXT_CACHE}" _CEF_CMAKE_CACHE)
    if(NOT _CEF_CMAKE_CACHE MATCHES "api_version:[A-Z]+=${JFN_CEF_API_VERSION}")
        message(FATAL_ERROR
            "Pre-built libcef_dll_wrapper at ${CEF_WRAPPER_PATH} was built "
            "with the wrong api_version (expected ${JFN_CEF_API_VERSION}, "
            "per ${_CEF_EXT_CACHE}). Rebuild it with "
            "-Dapi_version=${JFN_CEF_API_VERSION}.")
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

elseif(CEF_ROOT AND EXISTS "${CEF_ROOT}/include/cef_version.h")
    # Build CEF wrapper from CEF_ROOT

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
    elseif(APPLE)
        set(_WRAPPER_SEARCH_PATHS
            "${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.a"
            "${CEF_ROOT}/build/libcef_dll_wrapper/Release/libcef_dll_wrapper.a"
        )
    else() # Linux
        set(_WRAPPER_SEARCH_PATHS
            "${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.a"
        )
    endif()

    # Find wrapper in search paths
    set(CEF_WRAPPER_PATH "")
    foreach(_path ${_WRAPPER_SEARCH_PATHS})
        if(EXISTS "${_path}")
            set(CEF_WRAPPER_PATH "${_path}")
            break()
        endif()
    endforeach()

    # Validate cached wrapper was built with the api_version cef-rs expects.
    # A wrapper built without the pin defaults to CEF_API_VERSION_EXPERIMENTAL
    # (999999) and has different struct sizes than cef-rs — runtime wrap fails
    # with "invalid base.size". JFN_CEF_API_VERSION is set by
    # DetectCefApiVersion.cmake from cef-dll-sys's CEF_API_VERSION_LAST.
    if(CEF_WRAPPER_PATH)
        if(NOT EXISTS "${CEF_ROOT}/build/CMakeCache.txt")
            # Wrapper present but no cache to prove its api_version. A pre-built
            # wrapper dropped in by a packager without the cache file falls into
            # this branch and would otherwise be used silently. Force a rebuild
            # against the standard build/ layout so the pin is observable.
            message(STATUS
                "libcef_dll_wrapper present at ${CEF_WRAPPER_PATH} but no "
                "CMakeCache.txt at ${CEF_ROOT}/build to verify api_version; "
                "rebuilding to guarantee api_version=${JFN_CEF_API_VERSION}")
            file(REMOVE "${CEF_WRAPPER_PATH}")
            set(CEF_WRAPPER_PATH "")
        else()
            file(READ "${CEF_ROOT}/build/CMakeCache.txt" _CEF_CMAKE_CACHE)
            if(NOT _CEF_CMAKE_CACHE MATCHES "api_version:[A-Z]+=${JFN_CEF_API_VERSION}")
                message(STATUS "Cached libcef_dll_wrapper has wrong api_version, rebuilding")
                file(REMOVE "${CEF_WRAPPER_PATH}")
                file(REMOVE "${CEF_ROOT}/build/CMakeCache.txt")
                set(CEF_WRAPPER_PATH "")
            endif()
        endif()
    endif()

    # Build wrapper if not found
    if(NOT CEF_WRAPPER_PATH)
        message(STATUS "libcef_dll_wrapper not found, building...")
        # Pass generator and architecture settings to match the main build.
        # api_version pins the wrapper ABI to match cef-rs's binding ABI.
        set(_CEF_CMAKE_ARGS -B build -DCMAKE_BUILD_TYPE=Release -Dapi_version=${JFN_CEF_API_VERSION})
        if(CMAKE_GENERATOR)
            list(APPEND _CEF_CMAKE_ARGS -G "${CMAKE_GENERATOR}")
        endif()
        if((WIN32 AND CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64") OR
           (UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64"))
            list(APPEND _CEF_CMAKE_ARGS -DPROJECT_ARCH=arm64)
        endif()
        # Clear CFLAGS / CXXFLAGS for the sub-build. CEF's CMake sets its own
        # -W and -D flags (including -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2);
        # an inherited `-Wp,-D_FORTIFY_SOURCE=3` from a hardened build env
        # (e.g. flatpak's default CXXFLAGS) collides with that and trips
        # -Werror on `_FORTIFY_SOURCE redefined`. The wrapper is a small
        # static lib linked into our binary, so dropping the outer env's
        # hardening flags here is a no-op for the final binary's hardening.
        set(_CEF_SUBBUILD_ENV ${CMAKE_COMMAND} -E env --unset=CFLAGS --unset=CXXFLAGS --unset=CPPFLAGS)
        execute_process(
            COMMAND ${_CEF_SUBBUILD_ENV} ${CMAKE_COMMAND} ${_CEF_CMAKE_ARGS}
            WORKING_DIRECTORY ${CEF_ROOT}
            RESULT_VARIABLE CEF_CONFIG_RESULT
        )
        if(NOT CEF_CONFIG_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to configure CEF")
        endif()
        # Limit parallelism - CEF wrapper builds OOM with unlimited -j even on 16GB RAM
        # Windows multi-config generators need --config Release
        if(WIN32)
            set(_CEF_BUILD_CMD ${_CEF_SUBBUILD_ENV} ${CMAKE_COMMAND} --build build --target libcef_dll_wrapper --config Release -j2)
        else()
            set(_CEF_BUILD_CMD ${_CEF_SUBBUILD_ENV} ${CMAKE_COMMAND} --build build --target libcef_dll_wrapper -j2)
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

elseif(USE_SYSTEM_CEF)
    # Use system CEF from the "cef" package
    message(STATUS "Using system CEF from /usr/include/cef")

    if(NOT EXISTS "/usr/include/cef/include/cef_version.h")
        message(FATAL_ERROR "System CEF not found. Install the 'cef' package or set USE_SYSTEM_CEF=OFF")
    endif()

    # Read CEF version
    file(READ "/usr/include/cef/include/cef_version.h" CEF_VERSION_CONTENT)
    string(REGEX MATCH "CEF_VERSION \"([^\"]+)\"" _ ${CEF_VERSION_CONTENT})
    set(CEF_VERSION ${CMAKE_MATCH_1})
    set(CEF_IS_SYSTEM TRUE)
    message(STATUS "Found system CEF: ${CEF_VERSION}")

    set(CEF_INCLUDE_DIRS "/usr/include/cef" "/usr/include/cef/include" "/usr/src/cef")
    set(CEF_RESOURCE_DIR "/usr/lib/cef")
    set(CEF_RELEASE_DIR "/usr/lib/cef")

    # Build libcef_dll_wrapper from system sources.
    # The wrapper CMakeLists.txt references ../include/ relative to its location,
    # so we create a source tree with symlinks so those paths resolve correctly.
    # Version is embedded in the path so a CEF package upgrade triggers a rebuild.
    string(REGEX REPLACE "[^a-zA-Z0-9.]" "_" _CEF_VERSION_SAFE "${CEF_VERSION}")
    set(_WRAPPER_BUILD_DIR "${CMAKE_BINARY_DIR}/cef_wrapper_${_CEF_VERSION_SAFE}")
    set(_WRAPPER_SRC_DIR "${_WRAPPER_BUILD_DIR}/src")
    set(_WRAPPER_SEARCH_PATHS
        "${_WRAPPER_SRC_DIR}/build/libcef_dll/libcef_dll_wrapper.a"
        "${_WRAPPER_SRC_DIR}/build/libcef_dll_wrapper/libcef_dll_wrapper.a"
    )

    # Check if already built and not stale (source newer than wrapper)
    set(CEF_WRAPPER_PATH "")
    foreach(_path ${_WRAPPER_SEARCH_PATHS})
        if(EXISTS "${_path}")
            file(TIMESTAMP "${_path}" _WRAPPER_TS)
            file(TIMESTAMP "/usr/src/cef/libcef_dll" _SRC_TS)
            if(_SRC_TS STRGREATER _WRAPPER_TS)
                message(STATUS "System CEF sources are newer than cached wrapper, rebuilding...")
            else()
                set(CEF_WRAPPER_PATH "${_path}")
            endif()
            break()
        endif()
    endforeach()

    # Validate cached wrapper was built with the api_version cef-rs expects.
    # See DetectCefApiVersion.cmake for the failure mode if this drifts. The
    # cache file records api_version under CEF_API_VERSION_CHECK below, since
    # CEF's wrapper CMakeLists doesn't declare its own api_version cache var
    # in system-package builds (no cef_variables.cmake is shipped).
    if(CEF_WRAPPER_PATH AND EXISTS "${_WRAPPER_SRC_DIR}/build/CMakeCache.txt")
        file(READ "${_WRAPPER_SRC_DIR}/build/CMakeCache.txt" _CEF_CMAKE_CACHE)
        if(NOT _CEF_CMAKE_CACHE MATCHES "CEF_API_VERSION_CHECK:[A-Z]+=${JFN_CEF_API_VERSION}")
            message(STATUS "Cached libcef_dll_wrapper has wrong api_version, rebuilding")
            file(REMOVE "${CEF_WRAPPER_PATH}")
            file(REMOVE "${_WRAPPER_SRC_DIR}/build/CMakeCache.txt")
            set(CEF_WRAPPER_PATH "")
        endif()
    endif()

    if(NOT CEF_WRAPPER_PATH)
        message(STATUS "Building libcef_dll_wrapper from system sources...")
        file(MAKE_DIRECTORY "${_WRAPPER_SRC_DIR}")
        file(CREATE_LINK "/usr/include/cef/include" "${_WRAPPER_SRC_DIR}/include"
            SYMBOLIC RESULT _link_result)
        if(_link_result)
            message(FATAL_ERROR "Failed to symlink /usr/include/cef/include: ${_link_result}")
        endif()
        file(CREATE_LINK "/usr/src/cef/libcef_dll" "${_WRAPPER_SRC_DIR}/libcef_dll"
            SYMBOLIC RESULT _link_result)
        if(_link_result)
            message(FATAL_ERROR "Failed to symlink /usr/src/cef/libcef_dll. "
                "Is the 'cef' package fully installed?")
        endif()
        file(WRITE "${_WRAPPER_SRC_DIR}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.16)
project(cef_wrapper)
# Provide the macro expected by CEF's wrapper CMakeLists.txt
macro(SET_LIBRARY_TARGET_PROPERTIES target)
    target_include_directories(\${target} PRIVATE \"${_WRAPPER_SRC_DIR}\")
endmacro()
# Pin wrapper ABI to match cef-rs's binding ABI. CEF binary distributions
# handle this via cmake/cef_variables.cmake (which turns -Dapi_version=N into
# -DCEF_API_VERSION=N), but the system 'cef' package ships only libcef_dll/,
# so set the compile definition directly. CEF_API_VERSION_CHECK is also
# stored as a cache var so the cache-staleness check in FindCEF.cmake can
# detect a mismatch on later configures.
set(CEF_API_VERSION_CHECK \"\${CEF_API_VERSION_CHECK}\" CACHE STRING \"Pinned CEF API version\")
add_compile_definitions(CEF_API_VERSION=\${CEF_API_VERSION_CHECK})
add_subdirectory(libcef_dll)
")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -B build -DCMAKE_BUILD_TYPE=Release
                "-DCMAKE_C_FLAGS=-fPIC -ffile-prefix-map=${_WRAPPER_SRC_DIR}=cef_wrapper"
                "-DCMAKE_CXX_FLAGS=-fPIC -ffile-prefix-map=${_WRAPPER_SRC_DIR}=cef_wrapper"
                -DCMAKE_CXX_STANDARD=20
                -DCEF_API_VERSION_CHECK=${JFN_CEF_API_VERSION}
            WORKING_DIRECTORY "${_WRAPPER_SRC_DIR}"
            RESULT_VARIABLE _WRAPPER_CONFIG_RESULT
        )
        if(NOT _WRAPPER_CONFIG_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to configure libcef_dll_wrapper from system sources")
        endif()
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build build --target libcef_dll_wrapper -j2
            WORKING_DIRECTORY "${_WRAPPER_SRC_DIR}"
            RESULT_VARIABLE _WRAPPER_BUILD_RESULT
        )
        if(NOT _WRAPPER_BUILD_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to build libcef_dll_wrapper from system sources")
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

    set(CEF_LIBRARIES
        "${CEF_RELEASE_DIR}/libcef.so"
        "${CEF_WRAPPER_PATH}"
    )

else()
    message(FATAL_ERROR "CEF not found. Either:\n"
        "  - Install the 'cef' package (system CEF)\n"
        "  - Set EXTERNAL_CEF_DIR to a CEF installation\n"
        "  - Download CEF to third_party/cef/ (dev/tools/download_cef.py)")
endif()

set(CEF_FOUND TRUE)
