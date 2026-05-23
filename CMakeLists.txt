cmake_minimum_required(VERSION 3.21)
project(jellyfin-desktop LANGUAGES NONE)

# Pure-Rust binary. CMake is retained only to orchestrate the meson-built
# mpv submodule (when EXTERNAL_MPV_DIR is unset), invoke cargo to produce
# the final binary, stage runtime resources (CEF + libmpv) next to it,
# and run the per-platform install/CPack rules. All native code lives in
# the Rust workspace at src/Cargo.toml; libcef_dll_wrapper is built and
# linked by cef-dll-sys's build.rs.

# Version metadata (used by Info.plist on macOS; consumed inside the
# Rust binary via src/jfn_cef/build.rs which reads VERSION / CEF_VERSION
# directly).
file(READ "${CMAKE_SOURCE_DIR}/VERSION" APP_VERSION)
string(STRIP "${APP_VERSION}" APP_VERSION)
string(REGEX REPLACE "-.*$" "" APP_VERSION_NUMERIC "${APP_VERSION}")
if(NOT APP_VERSION_NUMERIC MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "VERSION must be MAJOR.MINOR.PATCH[-suffix]; got '${APP_VERSION}'")
endif()
set(APP_VERSION_MAJOR ${CMAKE_MATCH_1})
set(APP_VERSION_MINOR ${CMAKE_MATCH_2})
set(APP_VERSION_PATCH ${CMAKE_MATCH_3})
if(APP_VERSION MATCHES "-")
    execute_process(
        COMMAND git describe --always --dirty
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _APP_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _GIT_RESULT
    )
    if(_GIT_RESULT EQUAL 0 AND NOT _APP_GIT_HASH STREQUAL "")
        set(APP_VERSION_FULL "${APP_VERSION}+${_APP_GIT_HASH}")
    else()
        set(APP_VERSION_FULL "${APP_VERSION}")
    endif()
else()
    set(APP_VERSION_FULL "${APP_VERSION}")
endif()

# CEF resource discovery (needed only for staging resources next to the
# binary + install rules — libcef_dll_wrapper is built and linked by
# cef-dll-sys's build.rs during the cargo build).
#
# Precedence: EXTERNAL_CEF_DIR > USE_SYSTEM_CEF > CEF_ROOT (third_party/cef).
set(CEF_ROOT "${CMAKE_SOURCE_DIR}/third_party/cef" CACHE PATH "CEF binary distribution root")
set(EXTERNAL_CEF_DIR "" CACHE PATH "Path to external CEF installation")

set(_DEFAULT_USE_SYSTEM_CEF OFF)
if(EXISTS "/usr/include/cef/include/cef_version.h"
   AND NOT EXTERNAL_CEF_DIR
   AND NOT (CEF_ROOT AND EXISTS "${CEF_ROOT}/include/cef_version.h"))
    set(_DEFAULT_USE_SYSTEM_CEF ON)
endif()
option(USE_SYSTEM_CEF "Use system-installed CEF from the 'cef' package" ${_DEFAULT_USE_SYSTEM_CEF})

if(EXTERNAL_CEF_DIR)
    set(CEF_RESOURCE_DIR "${EXTERNAL_CEF_DIR}/lib")
    set(CEF_RELEASE_DIR  "${EXTERNAL_CEF_DIR}/lib")
    set(_CEF_VERSION_HEADER "${EXTERNAL_CEF_DIR}/include/cef_version.h")
elseif(USE_SYSTEM_CEF)
    set(CEF_RESOURCE_DIR "/usr/lib/cef")
    set(CEF_RELEASE_DIR  "/usr/lib/cef")
    set(_CEF_VERSION_HEADER "/usr/include/cef/include/cef_version.h")
elseif(CEF_ROOT AND EXISTS "${CEF_ROOT}/include/cef_version.h")
    set(CEF_RESOURCE_DIR "${CEF_ROOT}/Resources")
    set(CEF_RELEASE_DIR  "${CEF_ROOT}/Release")
    set(_CEF_VERSION_HEADER "${CEF_ROOT}/include/cef_version.h")
else()
    message(FATAL_ERROR "CEF not found. Set EXTERNAL_CEF_DIR, enable USE_SYSTEM_CEF, or populate third_party/cef (dev/tools/download_cef.py).")
endif()

file(READ "${_CEF_VERSION_HEADER}" CEF_VERSION_CONTENT)
string(REGEX MATCH "CEF_VERSION \"([^\"]+)\"" _ ${CEF_VERSION_CONTENT})
set(CEF_VERSION ${CMAKE_MATCH_1})
message(STATUS "Found CEF: ${CEF_VERSION}")

if(EXISTS "${CMAKE_SOURCE_DIR}/CEF_VERSION")
    file(READ "${CMAKE_SOURCE_DIR}/CEF_VERSION" _EXPECTED_CEF_VERSION)
    string(STRIP "${_EXPECTED_CEF_VERSION}" _EXPECTED_CEF_VERSION)
    if(NOT CEF_VERSION STREQUAL _EXPECTED_CEF_VERSION)
        if(USE_SYSTEM_CEF)
            message(WARNING
                "System CEF version does not match CEF_VERSION file:\n"
                "  CEF_VERSION file: ${_EXPECTED_CEF_VERSION}\n"
                "  System CEF:       ${CEF_VERSION}")
        else()
            message(FATAL_ERROR
                "CEF version mismatch:\n"
                "  CEF_VERSION file: ${_EXPECTED_CEF_VERSION}\n"
                "  Found CEF:        ${CEF_VERSION}")
        endif()
    endif()
endif()

# Locate cargo for the Rust workspace build.
find_program(CARGO_EXECUTABLE cargo REQUIRED)

# mpv build (meson submodule or external).
option(BUILD_MPV_CLI "Also build the standalone mpv CLI binary from the submodule" OFF)

set(_DEFAULT_EXTERNAL_MPV_DIR "")
if(EXISTS "/opt/jellyfin-desktop/libmpv/lib/libmpv.so")
    set(_DEFAULT_EXTERNAL_MPV_DIR "/opt/jellyfin-desktop/libmpv")
elseif(APPLE AND EXISTS "/opt/jellyfin-desktop/libmpv/lib/libmpv.dylib")
    set(_DEFAULT_EXTERNAL_MPV_DIR "/opt/jellyfin-desktop/libmpv")
endif()

set(EXTERNAL_MPV_DIR "${_DEFAULT_EXTERNAL_MPV_DIR}" CACHE PATH "Path to external mpv installation (must contain include/ and lib/)")

if(EXTERNAL_MPV_DIR)
    message(STATUS "Using external mpv from: ${EXTERNAL_MPV_DIR}")
    if(APPLE)
        set(MPV_LIBRARY "${EXTERNAL_MPV_DIR}/lib/libmpv.dylib")
    elseif(WIN32)
        set(MPV_LIBRARY "${EXTERNAL_MPV_DIR}/lib/mpv.lib")
    else()
        set(MPV_LIBRARY "${EXTERNAL_MPV_DIR}/lib/libmpv.so")
    endif()
    if(NOT EXISTS "${MPV_LIBRARY}")
        message(FATAL_ERROR "mpv library not found at ${MPV_LIBRARY}")
    endif()
else()
    set(MPV_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/mpv")
    set(MPV_BUILD_DIR "${CMAKE_BINARY_DIR}/mpv-build")
    if(APPLE)
        set(MPV_LIBRARY "${MPV_BUILD_DIR}/libmpv.dylib")
    elseif(WIN32)
        set(MPV_LIBRARY "${MPV_BUILD_DIR}/mpv.lib")
    else()
        set(MPV_LIBRARY "${MPV_BUILD_DIR}/libmpv.so")
    endif()
    if(BUILD_MPV_CLI)
        set(_MPV_CPLAYER "true")
    else()
        set(_MPV_CPLAYER "false")
    endif()
    if(NOT EXISTS "${MPV_BUILD_DIR}/build.ninja")
        message(STATUS "Configuring mpv with meson (cplayer=${_MPV_CPLAYER})...")
        execute_process(
            COMMAND meson setup ${MPV_BUILD_DIR} ${MPV_SOURCE_DIR}
                    --default-library=shared -Dlibmpv=true -Dcplayer=${_MPV_CPLAYER}
            RESULT_VARIABLE MPV_SETUP_RESULT
        )
        if(NOT MPV_SETUP_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to configure mpv with meson")
        endif()
    else()
        execute_process(
            COMMAND meson configure ${MPV_BUILD_DIR} -Dcplayer=${_MPV_CPLAYER}
            RESULT_VARIABLE MPV_RECONFIG_RESULT
        )
        if(NOT MPV_RECONFIG_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to reconfigure mpv cplayer option")
        endif()
    endif()
    add_custom_target(mpv_build ALL
        COMMAND meson compile -C ${MPV_BUILD_DIR}
        COMMENT "Building mpv (meson handles incremental builds)"
        BYPRODUCTS ${MPV_LIBRARY}
    )
endif()

# Rust workspace build. Single cargo invocation produces the final binary
# and (via cef-dll-sys's build.rs) stages CEF runtime files into the
# cargo target dir. The custom command copies the binary + libmpv into
# ${CMAKE_BINARY_DIR} so the bundle scripts find them at the same paths
# they used before.
set(JFN_RUST_DIR "${CMAKE_SOURCE_DIR}/src/jfn_rust")
set(JFN_RUST_WORKSPACE_DIR "${CMAKE_SOURCE_DIR}/src")
set(JFN_RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/jfn-rust-target")

if(WIN32)
    set(JFN_BIN_NAME "jellyfin-desktop.exe")
else()
    set(JFN_BIN_NAME "jellyfin-desktop")
endif()
set(JFN_BIN_SRC "${JFN_RUST_TARGET_DIR}/release/${JFN_BIN_NAME}")
set(JFN_BIN_DST "${CMAKE_BINARY_DIR}/${JFN_BIN_NAME}")

file(GLOB_RECURSE JFN_RUST_SOURCES CONFIGURE_DEPENDS
    "${JFN_RUST_WORKSPACE_DIR}/*/src/*.rs"
    "${JFN_RUST_WORKSPACE_DIR}/*/Cargo.toml"
    "${JFN_RUST_WORKSPACE_DIR}/Cargo.toml"
)
file(GLOB_RECURSE WEB_RESOURCES "${CMAKE_SOURCE_DIR}/src/web/*")

option(JFN_ENABLE_KDE_PALETTE "Build with KWin per-window titlebar color support" ON)
set(_JFN_CARGO_ARGS build --release --bin jellyfin-desktop --manifest-path ${JFN_RUST_DIR}/Cargo.toml)
if(NOT JFN_ENABLE_KDE_PALETTE)
    list(APPEND _JFN_CARGO_ARGS --no-default-features)
endif()

add_custom_command(
    OUTPUT ${JFN_BIN_DST}
    COMMAND ${CMAKE_COMMAND} -E env
        CARGO_TARGET_DIR=${JFN_RUST_TARGET_DIR}
        "EXTERNAL_MPV_DIR=${EXTERNAL_MPV_DIR}"
        ${CARGO_EXECUTABLE} ${_JFN_CARGO_ARGS}
    COMMAND ${CMAKE_COMMAND} -E copy ${JFN_BIN_SRC} ${JFN_BIN_DST}
    DEPENDS ${JFN_RUST_SOURCES} ${WEB_RESOURCES}
    COMMENT "Building jellyfin-desktop (Rust binary)"
    VERBATIM
)

# Driver target — `cmake --build build` produces ${JFN_BIN_DST} and
# stages runtime resources alongside it. Marked ALL so it's the default.
add_custom_target(jellyfin_desktop ALL DEPENDS ${JFN_BIN_DST})

if(NOT EXTERNAL_MPV_DIR)
    add_dependencies(jellyfin_desktop mpv_build)
endif()

# Stage CEF resources next to the binary. cef-dll-sys copies the runtime
# bits into the cargo target dir already; we additionally mirror them
# into ${CMAKE_BINARY_DIR} so the existing AppImage / bundle scripts
# (which look in build/) keep working. Skip when an
# external/system CEF is configured — those flow comes via install.
if(NOT EXTERNAL_CEF_DIR AND NOT USE_SYSTEM_CEF)
    if(APPLE)
        set(CEF_FRAMEWORK_NAME "Chromium Embedded Framework")
        set(CEF_FRAMEWORK_DIR "${CMAKE_BINARY_DIR}/Frameworks/${CEF_FRAMEWORK_NAME}.framework")
        set(NEW_INSTALL_NAME "@executable_path/Frameworks/${CEF_FRAMEWORK_NAME}.framework/${CEF_FRAMEWORK_NAME}")
        set(OLD_INSTALL_NAME "@executable_path/../Frameworks/${CEF_FRAMEWORK_NAME}.framework/${CEF_FRAMEWORK_NAME}")
        add_custom_command(TARGET jellyfin_desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CEF_RELEASE_DIR}/${CEF_FRAMEWORK_NAME}.framework"
                "${CEF_FRAMEWORK_DIR}"
            COMMAND install_name_tool -id "${NEW_INSTALL_NAME}" "${CEF_FRAMEWORK_DIR}/${CEF_FRAMEWORK_NAME}"
            COMMAND install_name_tool -change "${OLD_INSTALL_NAME}" "${NEW_INSTALL_NAME}" "${JFN_BIN_DST}"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "Frameworks/${CEF_FRAMEWORK_NAME}.framework/Libraries/libEGL.dylib"
                "${CMAKE_BINARY_DIR}/libEGL.dylib"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "Frameworks/${CEF_FRAMEWORK_NAME}.framework/Libraries/libGLESv2.dylib"
                "${CMAKE_BINARY_DIR}/libGLESv2.dylib"
            COMMENT "Staging CEF framework next to binary"
        )
    else()
        add_custom_command(TARGET jellyfin_desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CEF_RESOURCE_DIR}"
                "${CMAKE_BINARY_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CEF_RELEASE_DIR}"
                "${CMAKE_BINARY_DIR}"
        )
    endif()
endif()

# Stage libmpv next to the binary.
if(NOT EXTERNAL_MPV_DIR)
    if(APPLE)
        add_custom_command(TARGET jellyfin_desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${MPV_BUILD_DIR}/libmpv.2.dylib"
                "${CMAKE_BINARY_DIR}/libmpv.2.dylib"
            COMMAND install_name_tool -id "@executable_path/libmpv.2.dylib"
                "${CMAKE_BINARY_DIR}/libmpv.2.dylib"
            COMMAND install_name_tool -change "@rpath/libmpv.2.dylib"
                "@executable_path/libmpv.2.dylib" "${JFN_BIN_DST}"
        )
    elseif(WIN32)
        add_custom_command(TARGET jellyfin_desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${MPV_BUILD_DIR}/libmpv-2.dll"
                "${CMAKE_BINARY_DIR}/libmpv-2.dll"
        )
    else()
        add_custom_command(TARGET jellyfin_desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${MPV_BUILD_DIR}/libmpv.so.2"
                "${CMAKE_BINARY_DIR}/libmpv.so.2"
        )
    endif()
elseif(WIN32)
    file(GLOB _MPV_BUILD_DLLS "${EXTERNAL_MPV_DIR}/lib/*.dll")
    add_custom_command(TARGET jellyfin_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_MPV_BUILD_DLLS}
            "${CMAKE_BINARY_DIR}"
    )
endif()

# ---- Install rules ----------------------------------------------------
# Everything below assumes ${JFN_BIN_DST} + the post-build staged
# resources exist at ${CMAKE_BINARY_DIR}.

install(PROGRAMS ${JFN_BIN_DST} DESTINATION .)

if(WIN32)
    install(DIRECTORY "${CEF_RELEASE_DIR}/" DESTINATION .
        FILES_MATCHING PATTERN "*.dll" PATTERN "*.bin" PATTERN "*.json")
    install(DIRECTORY "${CEF_RESOURCE_DIR}/" DESTINATION .
        FILES_MATCHING PATTERN "*.pak" PATTERN "*.dat")
    install(DIRECTORY "${CEF_RESOURCE_DIR}/locales" DESTINATION .)
    if(EXTERNAL_MPV_DIR)
        file(GLOB _MPV_DLLS "${EXTERNAL_MPV_DIR}/lib/*.dll")
        install(FILES ${_MPV_DLLS} DESTINATION .)
    endif()
elseif(APPLE)
    set(APP_NAME "Jellyfin Desktop.app")
    install(PROGRAMS ${JFN_BIN_DST} DESTINATION "${APP_NAME}/Contents/MacOS")
    install(FILES "${CMAKE_BINARY_DIR}/libmpv.2.dylib"
        DESTINATION "${APP_NAME}/Contents/MacOS"
    )
    install(DIRECTORY "${CMAKE_BINARY_DIR}/Frameworks/Chromium Embedded Framework.framework"
        DESTINATION "${APP_NAME}/Contents/Frameworks"
    )
    install(CODE "
        execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
            \"../Frameworks/Chromium Embedded Framework.framework/Libraries/libEGL.dylib\"
            \"\${CMAKE_INSTALL_PREFIX}/${APP_NAME}/Contents/MacOS/libEGL.dylib\")
        execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
            \"../Frameworks/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib\"
            \"\${CMAKE_INSTALL_PREFIX}/${APP_NAME}/Contents/MacOS/libGLESv2.dylib\")
    ")
    configure_file(
        "${CMAKE_SOURCE_DIR}/resources/macos/Info.plist.in"
        "${CMAKE_BINARY_DIR}/Info.plist"
        @ONLY
    )
    install(FILES "${CMAKE_BINARY_DIR}/Info.plist"
        DESTINATION "${APP_NAME}/Contents"
    )
    if(EXISTS "${CMAKE_SOURCE_DIR}/resources/macos/AppIcon.icns")
        install(FILES "${CMAKE_SOURCE_DIR}/resources/macos/AppIcon.icns"
            DESTINATION "${APP_NAME}/Contents/Resources"
        )
    endif()
    install(DIRECTORY DESTINATION "${APP_NAME}/Contents/Resources")
    install(FILES "${CMAKE_SOURCE_DIR}/resources/macos/MoltenVK_icd.json"
        DESTINATION "${APP_NAME}/Contents/Resources/vulkan/icd.d"
    )
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/CompleteBundleMac.cmake.in"
        "${CMAKE_BINARY_DIR}/CompleteBundleMac.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_BINARY_DIR}/CompleteBundleMac.cmake")
else()
    install(DIRECTORY "${CEF_RELEASE_DIR}/" DESTINATION .
        FILES_MATCHING PATTERN "*.so*" PATTERN "*.bin")
    install(DIRECTORY "${CEF_RESOURCE_DIR}/" DESTINATION .
        FILES_MATCHING PATTERN "*.pak" PATTERN "*.dat")
    install(DIRECTORY "${CEF_RESOURCE_DIR}/locales" DESTINATION .)
    if(EXTERNAL_MPV_DIR)
        install(FILES "${EXTERNAL_MPV_DIR}/lib/libmpv.so" DESTINATION .)
    endif()
endif()

# CPack for creating distribution packages.
set(CPACK_PACKAGE_NAME "jellyfin-desktop")
set(CPACK_PACKAGE_VENDOR "Jellyfin")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Jellyfin Desktop Client")
set(CPACK_PACKAGE_VERSION "${APP_VERSION}")
if(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|ARM64|arm64)")
        set(_WIN_ARCH "arm64")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_WIN_ARCH "x64")
    else()
        set(_WIN_ARCH "x86")
    endif()
    set(CPACK_PACKAGE_FILE_NAME "JellyfinDesktop-${APP_VERSION_FULL}-windows-${_WIN_ARCH}")
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
