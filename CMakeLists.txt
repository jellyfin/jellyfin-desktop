cmake_minimum_required(VERSION 3.21)
project(jellyfin-desktop LANGUAGES C CXX)

# Enable Objective-C++ on macOS
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Version info (checks VERSION file and git hash each build, only writes if changed)
add_custom_target(generate_version ALL
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR="${CMAKE_SOURCE_DIR}"
        -DBINARY_DIR="${CMAKE_BINARY_DIR}"
        -P "${CMAKE_SOURCE_DIR}/cmake/GenerateVersion.cmake"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/src/version.h"
    COMMENT "Checking version info"
)
include_directories("${CMAKE_BINARY_DIR}/src")

# Use static runtime on Windows to match CEF wrapper (which uses /MT)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_definitions(NOMINMAX)
endif()

# Match CEF's compile flags for ABI compatibility
if(UNIX AND NOT APPLE)
    add_compile_options(
        -fno-strict-aliasing
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
        -fPIC
        -fstack-protector
        -funwind-tables
        -fvisibility=hidden
        --param=ssp-buffer-size=4
        -pipe
        -pthread
    )
endif()

# CEF configuration
set(CEF_ROOT "${CMAKE_SOURCE_DIR}/third_party/cef" CACHE PATH "CEF binary distribution root")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(CEF REQUIRED)

# Verify CEF version matches CEF_VERSION file
if(EXISTS "${CMAKE_SOURCE_DIR}/CEF_VERSION")
    file(READ "${CMAKE_SOURCE_DIR}/CEF_VERSION" _EXPECTED_CEF_VERSION)
    string(STRIP "${_EXPECTED_CEF_VERSION}" _EXPECTED_CEF_VERSION)
    if(NOT CEF_VERSION STREQUAL _EXPECTED_CEF_VERSION)
        if(CEF_IS_SYSTEM)
            message(WARNING
                "System CEF version does not match CEF_VERSION file:\n"
                "  CEF_VERSION file: ${_EXPECTED_CEF_VERSION}\n"
                "  System CEF:       ${CEF_VERSION}\n"
                "  This may cause build failures or runtime issues.")
        else()
            message(FATAL_ERROR
                "CEF version mismatch:\n"
                "  CEF_VERSION file: ${_EXPECTED_CEF_VERSION}\n"
                "  Found CEF:        ${CEF_VERSION}")
        endif()
    endif()
endif()


# Platform-specific dependencies
if(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(MEDIAPLAYER_FRAMEWORK MediaPlayer REQUIRED)

    set(PLATFORM_SOURCES
        src/platform/macos.mm
        src/input/input_macos.mm
        src/wake_event_macos.cpp
        src/player/media_session.cpp
        src/player/macos/media_session_macos.mm
        src/player/media_session_thread.cpp
    )

    # LetsMove - prompts user to move app to /Applications (clears quarantine)
    add_subdirectory(third_party/letsmove)
    set(PLATFORM_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/letsmove)

    set(PLATFORM_LIBRARIES
        ${COCOA_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${IOSURFACE_FRAMEWORK}
        ${IOKIT_FRAMEWORK}
        ${MEDIAPLAYER_FRAMEWORK}
    )
elseif(WIN32)
    set(PLATFORM_SOURCES
        src/platform/windows.cpp
        src/input/input_windows.cpp
        src/player/media_session.cpp
        src/player/windows/media_session_windows.cpp
        src/player/media_session_thread.cpp
        ${CMAKE_SOURCE_DIR}/resources/win/iconres.rc
    )
    set(PLATFORM_LIBRARIES
        dwmapi
        d3d11
        dxgi
        dcomp
        crypt32
        windowsapp
    )
else()
    # Linux: Wayland + X11 backends (runtime selection)
    find_package(PkgConfig REQUIRED)

    # Shared Linux deps
    pkg_check_modules(SYSTEMD REQUIRED libsystemd)
    pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
    find_package(OpenGL REQUIRED COMPONENTS EGL)

    # Wayland deps
    pkg_check_modules(LIBDRM REQUIRED libdrm)
    pkg_check_modules(WAYLAND REQUIRED wayland-client)
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)

    # X11 (optional)
    option(ENABLE_X11 "X11 backend support" ON)
    if(ENABLE_X11)
        pkg_check_modules(XCB REQUIRED xcb)
        pkg_check_modules(XCB_SHM REQUIRED xcb-shm)
        pkg_check_modules(XCB_SHAPE REQUIRED xcb-shape)
        pkg_check_modules(XCB_CURSOR REQUIRED xcb-cursor)
        pkg_check_modules(XCB_XKB REQUIRED xcb-xkb)
        pkg_check_modules(XKBCOMMON_X11 REQUIRED xkbcommon-x11)
    endif()

    # Find wayland-scanner
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

    # Generate Wayland protocol sources
    set(WAYLAND_GENERATED_DIR "${CMAKE_BINARY_DIR}/wayland-protocols")
    file(MAKE_DIRECTORY ${WAYLAND_GENERATED_DIR})

    set(LINUX_DMABUF_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml")
    set(LINUX_DMABUF_CLIENT_H "${WAYLAND_GENERATED_DIR}/linux-dmabuf-v1-client.h")
    set(LINUX_DMABUF_CODE_C "${WAYLAND_GENERATED_DIR}/linux-dmabuf-v1-code.c")

    set(VIEWPORTER_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml")
    set(VIEWPORTER_CLIENT_H "${WAYLAND_GENERATED_DIR}/viewporter-client.h")
    set(VIEWPORTER_CODE_C "${WAYLAND_GENERATED_DIR}/viewporter-code.c")

    set(ALPHA_MODIFIER_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/staging/alpha-modifier/alpha-modifier-v1.xml")
    set(ALPHA_MODIFIER_CLIENT_H "${WAYLAND_GENERATED_DIR}/alpha-modifier-v1-client.h")
    set(ALPHA_MODIFIER_CODE_C "${WAYLAND_GENERATED_DIR}/alpha-modifier-v1-code.c")

    set(CURSOR_SHAPE_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/staging/cursor-shape/cursor-shape-v1.xml")
    set(CURSOR_SHAPE_CLIENT_H "${WAYLAND_GENERATED_DIR}/cursor-shape-v1-client.h")
    set(CURSOR_SHAPE_CODE_C "${WAYLAND_GENERATED_DIR}/cursor-shape-v1-code.c")

    set(EXT_DATA_CONTROL_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/staging/ext-data-control/ext-data-control-v1.xml")
    set(EXT_DATA_CONTROL_CLIENT_H "${WAYLAND_GENERATED_DIR}/ext-data-control-v1-client.h")
    set(EXT_DATA_CONTROL_CODE_C "${WAYLAND_GENERATED_DIR}/ext-data-control-v1-code.c")

    # tablet-v2 interface symbols are referenced by cursor-shape-v1 generated code
    set(TABLET_V2_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/stable/tablet/tablet-v2.xml")
    set(TABLET_V2_CLIENT_H "${WAYLAND_GENERATED_DIR}/tablet-v2-client.h")
    set(TABLET_V2_CODE_C "${WAYLAND_GENERATED_DIR}/tablet-v2-code.c")

    # KDE server decoration palette (per-window titlebar colors on KWin)
    option(ENABLE_KDE_PALETTE "KDE/KWin per-window titlebar color support" ON)
    if(ENABLE_KDE_PALETTE)
        find_file(DECO_PALETTE_PROTOCOL server-decoration-palette.xml
            PATHS /usr/share/plasma-wayland-protocols
                  /app/share/plasma-wayland-protocols
            REQUIRED)
        set(DECO_PALETTE_CLIENT_H "${WAYLAND_GENERATED_DIR}/server-decoration-palette-client.h")
        set(DECO_PALETTE_CODE_C "${WAYLAND_GENERATED_DIR}/server-decoration-palette-code.c")
    endif()

    add_custom_command(
        OUTPUT ${LINUX_DMABUF_CLIENT_H}
        COMMAND ${WAYLAND_SCANNER} client-header ${LINUX_DMABUF_PROTOCOL} ${LINUX_DMABUF_CLIENT_H}
        DEPENDS ${LINUX_DMABUF_PROTOCOL}
        COMMENT "Generating linux-dmabuf-v1 client header"
    )
    add_custom_command(
        OUTPUT ${LINUX_DMABUF_CODE_C}
        COMMAND ${WAYLAND_SCANNER} private-code ${LINUX_DMABUF_PROTOCOL} ${LINUX_DMABUF_CODE_C}
        DEPENDS ${LINUX_DMABUF_PROTOCOL}
        COMMENT "Generating linux-dmabuf-v1 code"
    )
    add_custom_command(
        OUTPUT ${VIEWPORTER_CLIENT_H}
        COMMAND ${WAYLAND_SCANNER} client-header ${VIEWPORTER_PROTOCOL} ${VIEWPORTER_CLIENT_H}
        DEPENDS ${VIEWPORTER_PROTOCOL}
        COMMENT "Generating viewporter client header"
    )
    add_custom_command(
        OUTPUT ${VIEWPORTER_CODE_C}
        COMMAND ${WAYLAND_SCANNER} private-code ${VIEWPORTER_PROTOCOL} ${VIEWPORTER_CODE_C}
        DEPENDS ${VIEWPORTER_PROTOCOL}
        COMMENT "Generating viewporter code"
    )
    add_custom_command(
        OUTPUT ${ALPHA_MODIFIER_CLIENT_H}
        COMMAND ${WAYLAND_SCANNER} client-header ${ALPHA_MODIFIER_PROTOCOL} ${ALPHA_MODIFIER_CLIENT_H}
        DEPENDS ${ALPHA_MODIFIER_PROTOCOL}
        COMMENT "Generating alpha-modifier-v1 client header"
    )
    add_custom_command(
        OUTPUT ${ALPHA_MODIFIER_CODE_C}
        COMMAND ${WAYLAND_SCANNER} private-code ${ALPHA_MODIFIER_PROTOCOL} ${ALPHA_MODIFIER_CODE_C}
        DEPENDS ${ALPHA_MODIFIER_PROTOCOL}
        COMMENT "Generating alpha-modifier-v1 code"
    )
    add_custom_command(
        OUTPUT ${CURSOR_SHAPE_CLIENT_H}
        COMMAND ${WAYLAND_SCANNER} client-header ${CURSOR_SHAPE_PROTOCOL} ${CURSOR_SHAPE_CLIENT_H}
        DEPENDS ${CURSOR_SHAPE_PROTOCOL}
        COMMENT "Generating cursor-shape-v1 client header"
    )
    add_custom_command(
        OUTPUT ${CURSOR_SHAPE_CODE_C}
        COMMAND ${WAYLAND_SCANNER} private-code ${CURSOR_SHAPE_PROTOCOL} ${CURSOR_SHAPE_CODE_C}
        DEPENDS ${CURSOR_SHAPE_PROTOCOL}
        COMMENT "Generating cursor-shape-v1 code"
    )
    add_custom_command(
        OUTPUT ${EXT_DATA_CONTROL_CLIENT_H}
        COMMAND ${WAYLAND_SCANNER} client-header ${EXT_DATA_CONTROL_PROTOCOL} ${EXT_DATA_CONTROL_CLIENT_H}
        DEPENDS ${EXT_DATA_CONTROL_PROTOCOL}
        COMMENT "Generating ext-data-control-v1 client header"
    )
    add_custom_command(
        OUTPUT ${EXT_DATA_CONTROL_CODE_C}
        COMMAND ${WAYLAND_SCANNER} private-code ${EXT_DATA_CONTROL_PROTOCOL} ${EXT_DATA_CONTROL_CODE_C}
        DEPENDS ${EXT_DATA_CONTROL_PROTOCOL}
        COMMENT "Generating ext-data-control-v1 code"
    )
    add_custom_command(
        OUTPUT ${TABLET_V2_CLIENT_H}
        COMMAND ${WAYLAND_SCANNER} client-header ${TABLET_V2_PROTOCOL} ${TABLET_V2_CLIENT_H}
        DEPENDS ${TABLET_V2_PROTOCOL}
        COMMENT "Generating tablet-v2 client header"
    )
    add_custom_command(
        OUTPUT ${TABLET_V2_CODE_C}
        COMMAND ${WAYLAND_SCANNER} private-code ${TABLET_V2_PROTOCOL} ${TABLET_V2_CODE_C}
        DEPENDS ${TABLET_V2_PROTOCOL}
        COMMENT "Generating tablet-v2 code"
    )
    if(ENABLE_KDE_PALETTE)
        add_custom_command(
            OUTPUT ${DECO_PALETTE_CLIENT_H}
            COMMAND ${WAYLAND_SCANNER} client-header ${DECO_PALETTE_PROTOCOL} ${DECO_PALETTE_CLIENT_H}
            DEPENDS ${DECO_PALETTE_PROTOCOL}
            COMMENT "Generating server-decoration-palette client header"
        )
        add_custom_command(
            OUTPUT ${DECO_PALETTE_CODE_C}
            COMMAND ${WAYLAND_SCANNER} private-code ${DECO_PALETTE_PROTOCOL} ${DECO_PALETTE_CODE_C}
            DEPENDS ${DECO_PALETTE_PROTOCOL}
            COMMENT "Generating server-decoration-palette code"
        )
    endif()

    set(PLATFORM_SOURCES
        # Shared Linux
        src/platform/idle_inhibit_linux.cpp
        src/wake_event_linux.cpp
        src/player/media_session.cpp
        src/player/mpris/media_session_mpris.cpp
        src/player/media_session_thread.cpp
        # Wayland
        src/platform/wayland.cpp
        src/clipboard/wayland.cpp
        src/input/input_wayland.cpp
        # Generated Wayland protocols
        ${LINUX_DMABUF_CODE_C}
        ${LINUX_DMABUF_CLIENT_H}
        ${VIEWPORTER_CODE_C}
        ${VIEWPORTER_CLIENT_H}
        ${ALPHA_MODIFIER_CODE_C}
        ${ALPHA_MODIFIER_CLIENT_H}
        ${CURSOR_SHAPE_CODE_C}
        ${CURSOR_SHAPE_CLIENT_H}
        ${EXT_DATA_CONTROL_CODE_C}
        ${EXT_DATA_CONTROL_CLIENT_H}
        ${TABLET_V2_CODE_C}
        ${TABLET_V2_CLIENT_H}
    )
    if(ENABLE_KDE_PALETTE)
        list(APPEND PLATFORM_SOURCES
            ${DECO_PALETTE_CODE_C}
            ${DECO_PALETTE_CLIENT_H}
        )
        add_compile_definitions(HAVE_KDE_DECORATION_PALETTE)
    endif()
    if(ENABLE_X11)
        list(APPEND PLATFORM_SOURCES
            src/platform/x11.cpp
            src/input/input_x11.cpp
        )
        add_compile_definitions(HAVE_X11)
    endif()
    set(PLATFORM_LIBRARIES
        ${WAYLAND_LIBRARIES}
        ${SYSTEMD_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
        OpenGL::EGL
        ${CMAKE_DL_LIBS}
    )
    if(ENABLE_X11)
        list(APPEND PLATFORM_LIBRARIES
            ${XCB_LIBRARIES}
            ${XCB_SHM_LIBRARIES}
            ${XCB_SHAPE_LIBRARIES}
            ${XCB_CURSOR_LIBRARIES}
            ${XCB_XKB_LIBRARIES}
            ${XKBCOMMON_X11_LIBRARIES}
        )
    endif()
    set(PLATFORM_INCLUDE_DIRS
        ${WAYLAND_INCLUDE_DIRS}
        ${LIBDRM_INCLUDE_DIRS}
        ${SYSTEMD_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
        ${WAYLAND_GENERATED_DIR}
    )
    if(ENABLE_X11)
        list(APPEND PLATFORM_INCLUDE_DIRS
            ${XCB_INCLUDE_DIRS}
            ${XCB_SHM_INCLUDE_DIRS}
            ${XCB_SHAPE_INCLUDE_DIRS}
            ${XCB_CURSOR_INCLUDE_DIRS}
            ${XCB_XKB_INCLUDE_DIRS}
            ${XKBCOMMON_X11_INCLUDE_DIRS}
        )
    endif()
endif()

# mpv configuration
# Auto-detect external mpv from /opt if it exists
set(_DEFAULT_EXTERNAL_MPV_DIR "")
if(EXISTS "/opt/jellyfin-desktop/libmpv/lib/libmpv.so")
    set(_DEFAULT_EXTERNAL_MPV_DIR "/opt/jellyfin-desktop/libmpv")
elseif(APPLE AND EXISTS "/opt/jellyfin-desktop/libmpv/lib/libmpv.dylib")
    set(_DEFAULT_EXTERNAL_MPV_DIR "/opt/jellyfin-desktop/libmpv")
endif()

set(EXTERNAL_MPV_DIR "${_DEFAULT_EXTERNAL_MPV_DIR}" CACHE PATH "Path to external mpv installation (must contain include/ and lib/)")

if(EXTERNAL_MPV_DIR)
    message(STATUS "Using external mpv from: ${EXTERNAL_MPV_DIR}")
    set(MPV_INCLUDE_DIRS "${EXTERNAL_MPV_DIR}/include")
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
    set(MPV_LIBRARIES "${MPV_LIBRARY}")
else()
    # Build mpv from submodule using meson
    set(MPV_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/mpv")
    set(MPV_BUILD_DIR "${MPV_SOURCE_DIR}/build")

    if(APPLE)
        set(MPV_LIBRARY "${MPV_BUILD_DIR}/libmpv.dylib")
    elseif(WIN32)
        set(MPV_LIBRARY "${MPV_BUILD_DIR}/mpv.lib")
    else()
        set(MPV_LIBRARY "${MPV_BUILD_DIR}/libmpv.so")
    endif()

    # Configure meson if not yet configured
    if(NOT EXISTS "${MPV_BUILD_DIR}/build.ninja")
        message(STATUS "Configuring mpv with meson...")
        execute_process(
            COMMAND meson setup build --default-library=shared -Dlibmpv=true
            WORKING_DIRECTORY ${MPV_SOURCE_DIR}
            RESULT_VARIABLE MPV_SETUP_RESULT
        )
        if(NOT MPV_SETUP_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to configure mpv with meson")
        endif()
    endif()

    # Custom target that always invokes meson compile (meson handles incremental builds)
    add_custom_target(mpv_build
        COMMAND meson compile -C build
        WORKING_DIRECTORY ${MPV_SOURCE_DIR}
        COMMENT "Building mpv (meson handles incremental builds)"
        BYPRODUCTS ${MPV_LIBRARY}
    )

    set(MPV_INCLUDE_DIRS "${MPV_SOURCE_DIR}/include")
    set(MPV_LIBRARIES "${MPV_LIBRARY}")
endif()

# Embed JS shims into generated header
set(JS_SHIMS
    ${CMAKE_SOURCE_DIR}/src/web/native-shim.js
    ${CMAKE_SOURCE_DIR}/src/web/mpv-player-core.js
    ${CMAKE_SOURCE_DIR}/src/web/mpv-video-player.js
    ${CMAKE_SOURCE_DIR}/src/web/mpv-audio-player.js
    ${CMAKE_SOURCE_DIR}/src/web/input-plugin.js
    ${CMAKE_SOURCE_DIR}/src/web/context-menu.js
)
set(EMBEDDED_JS_HEADER ${CMAKE_BINARY_DIR}/generated/embedded_js.h)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)

add_custom_command(
    OUTPUT ${EMBEDDED_JS_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DJS_FILES="${JS_SHIMS}"
        -DOUTPUT_FILE=${EMBEDDED_JS_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_resources.cmake
    DEPENDS ${JS_SHIMS}
    COMMENT "Embedding JS shims"
)
add_custom_target(embedded_js DEPENDS ${EMBEDDED_JS_HEADER})

# Embed ALL web resources into generated header
set(EMBEDDED_RESOURCES_HEADER ${CMAKE_BINARY_DIR}/generated/embedded_resources.h)
file(GLOB_RECURSE WEB_RESOURCE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/web/*")

add_custom_command(
    OUTPUT ${EMBEDDED_RESOURCES_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DRESOURCE_DIR="${CMAKE_SOURCE_DIR}/src/web"
        -DOUTPUT_FILE=${EMBEDDED_RESOURCES_HEADER}
        -DBASE_URL="resources"
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_all_resources.cmake
    DEPENDS ${WEB_RESOURCE_FILES}
    COMMENT "Embedding web resources"
)
add_custom_target(embedded_resources DEPENDS ${EMBEDDED_RESOURCES_HEADER})

# Common sources
if(WIN32)
    set(COMMON_SOURCES
        src/main.cpp
        src/logging.cpp
        src/single_instance.cpp
        src/mpv/event.cpp
        src/cef/cef_app.cpp
        src/cef/cef_client.cpp
        src/cef/resource_handler.cpp
        src/input/dispatch.cpp
        src/input/hotkeys.cpp
        src/cjson/cJSON.c
        src/settings.cpp
        src/wake_event_windows.cpp
    )
else()
    # Linux + macOS: shared main with platform abstraction
    set(COMMON_SOURCES
        src/main.cpp
        src/logging.cpp
        src/mpv/event.cpp
        src/cef/cef_app.cpp
        src/cef/cef_client.cpp
        src/cef/resource_handler.cpp
        src/input/dispatch.cpp
        src/input/hotkeys.cpp
        src/single_instance.cpp
        src/settings.cpp
        src/cjson/cJSON.c
    )
endif()

add_executable(jellyfin-desktop
    ${COMMON_SOURCES}
    ${PLATFORM_SOURCES}
)

# Windows: hide console window (use WinMain entry via mainCRTStartup)
if(WIN32)
    if(MSVC)
        target_link_options(jellyfin-desktop PRIVATE
            /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
        )
    else()
        target_link_options(jellyfin-desktop PRIVATE -mwindows)
    endif()
endif()

# Ensure JS shims are embedded before compiling cef_app.cpp
add_dependencies(jellyfin-desktop embedded_js embedded_resources generate_version)

target_include_directories(jellyfin-desktop PRIVATE
    ${CEF_INCLUDE_DIRS}
    ${MPV_INCLUDE_DIRS}
    ${PLATFORM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(jellyfin-desktop PRIVATE
    ${CEF_LIBRARIES}
    ${MPV_LIBRARIES}
    ${PLATFORM_LIBRARIES}
)

# When using external/system CEF, tell the app where to find resources
if(EXTERNAL_CEF_DIR OR USE_SYSTEM_CEF)
    target_compile_definitions(jellyfin-desktop PRIVATE
        CEF_RESOURCES_DIR="${CEF_RESOURCE_DIR}"
    )
endif()

# Enable ARC for Objective-C++ files on macOS
if(APPLE)
    set_source_files_properties(
        src/platform/macos.mm
        src/input/input_macos.mm
        src/player/macos/media_session_macos.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# Linux RPATH configuration
if(UNIX AND NOT APPLE)
    set(RPATH_DIRS "$ORIGIN")
    if(EXTERNAL_MPV_DIR)
        list(APPEND RPATH_DIRS "${EXTERNAL_MPV_DIR}/lib")
    endif()
    if(USE_SYSTEM_CEF OR EXTERNAL_CEF_DIR)
        list(APPEND RPATH_DIRS "${CEF_RELEASE_DIR}")
    endif()
    list(JOIN RPATH_DIRS ";" RPATH_STRING)
    set_target_properties(jellyfin-desktop PROPERTIES
        INSTALL_RPATH "${RPATH_STRING}"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# Copy CEF resources to build directory (only when not using external/system CEF)
if(NOT EXTERNAL_CEF_DIR AND NOT USE_SYSTEM_CEF)
    if(APPLE)
        # macOS: Copy framework to build/Frameworks/ and fix install_name paths
        # CEF ships with @executable_path/../Frameworks/... but we want @executable_path/Frameworks/...
        set(CEF_FRAMEWORK_NAME "Chromium Embedded Framework")
        set(CEF_FRAMEWORK_DIR "${CMAKE_BINARY_DIR}/Frameworks/${CEF_FRAMEWORK_NAME}.framework")
        set(CEF_FRAMEWORK_LIB "${CEF_FRAMEWORK_DIR}/${CEF_FRAMEWORK_NAME}")
        set(OLD_INSTALL_NAME "@executable_path/../Frameworks/${CEF_FRAMEWORK_NAME}.framework/${CEF_FRAMEWORK_NAME}")
        set(NEW_INSTALL_NAME "@executable_path/Frameworks/${CEF_FRAMEWORK_NAME}.framework/${CEF_FRAMEWORK_NAME}")

        add_custom_command(TARGET jellyfin-desktop POST_BUILD
            # Copy framework
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CEF_RELEASE_DIR}/${CEF_FRAMEWORK_NAME}.framework"
                "${CEF_FRAMEWORK_DIR}"
            # Fix framework's install_name
            COMMAND install_name_tool -id "${NEW_INSTALL_NAME}" "${CEF_FRAMEWORK_LIB}"
            # Fix executable's reference to framework
            COMMAND install_name_tool -change "${OLD_INSTALL_NAME}" "${NEW_INSTALL_NAME}" "$<TARGET_FILE:jellyfin-desktop>"
            # Symlink GL libraries next to executable (CEF GPU compositor needs them at executable path)
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "Frameworks/${CEF_FRAMEWORK_NAME}.framework/Libraries/libEGL.dylib"
                "${CMAKE_BINARY_DIR}/libEGL.dylib"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "Frameworks/${CEF_FRAMEWORK_NAME}.framework/Libraries/libGLESv2.dylib"
                "${CMAKE_BINARY_DIR}/libGLESv2.dylib"
            COMMENT "Copying CEF framework and fixing install_name paths"
        )
    else()
        # Linux: Separate Resources and Release directories
        add_custom_command(TARGET jellyfin-desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CEF_RESOURCE_DIR}"
                "$<TARGET_FILE_DIR:jellyfin-desktop>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CEF_RELEASE_DIR}"
                "$<TARGET_FILE_DIR:jellyfin-desktop>"
        )
    endif()
endif()

if(NOT EXTERNAL_MPV_DIR)
    add_dependencies(jellyfin-desktop mpv_build)
endif()

# Copy libmpv to build directory
if(NOT EXTERNAL_MPV_DIR)
    if(APPLE)
        add_custom_command(TARGET jellyfin-desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${MPV_BUILD_DIR}/libmpv.2.dylib"
                "$<TARGET_FILE_DIR:jellyfin-desktop>/libmpv.2.dylib"
            # Fix libmpv install_name to load from executable directory
            COMMAND install_name_tool -id "@executable_path/libmpv.2.dylib"
                "$<TARGET_FILE_DIR:jellyfin-desktop>/libmpv.2.dylib"
            # Fix executable to use local libmpv
            COMMAND install_name_tool -change "@rpath/libmpv.2.dylib"
                "@executable_path/libmpv.2.dylib" "$<TARGET_FILE:jellyfin-desktop>"
        )
    elseif(WIN32)
        add_custom_command(TARGET jellyfin-desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${MPV_BUILD_DIR}/libmpv-2.dll"
                "$<TARGET_FILE_DIR:jellyfin-desktop>/libmpv-2.dll"
        )
    else()
        add_custom_command(TARGET jellyfin-desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${MPV_BUILD_DIR}/libmpv.so.2"
                "$<TARGET_FILE_DIR:jellyfin-desktop>/libmpv.so.2"
        )
    endif()
elseif(WIN32)
    # External mpv: copy all DLLs (libmpv + dependencies) to build directory
    file(GLOB _MPV_BUILD_DLLS "${EXTERNAL_MPV_DIR}/lib/*.dll")
    add_custom_command(TARGET jellyfin-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_MPV_BUILD_DLLS}
            "$<TARGET_FILE_DIR:jellyfin-desktop>"
    )
endif()

# Install rules
install(TARGETS jellyfin-desktop RUNTIME DESTINATION .)

if(WIN32)
    # CEF binaries and resources
    install(DIRECTORY "${CEF_RELEASE_DIR}/" DESTINATION .
        FILES_MATCHING PATTERN "*.dll" PATTERN "*.bin" PATTERN "*.json")
    install(DIRECTORY "${CEF_RESOURCE_DIR}/" DESTINATION .
        FILES_MATCHING PATTERN "*.pak" PATTERN "*.dat")
    install(DIRECTORY "${CEF_RESOURCE_DIR}/locales" DESTINATION .)

    # libmpv and its runtime dependencies
    if(EXTERNAL_MPV_DIR)
        file(GLOB _MPV_DLLS "${EXTERNAL_MPV_DIR}/lib/*.dll")
        install(FILES ${_MPV_DLLS} DESTINATION .)
    endif()
elseif(APPLE)
    # macOS app bundle installation
    set(APP_NAME "Jellyfin Desktop.app")
    set(BUNDLE_CONTENTS "${CMAKE_INSTALL_PREFIX}/${APP_NAME}/Contents")

    # Install executable
    install(PROGRAMS $<TARGET_FILE:jellyfin-desktop>
        DESTINATION "${APP_NAME}/Contents/MacOS"
    )

    # Install libmpv
    install(FILES "${CMAKE_BINARY_DIR}/libmpv.2.dylib"
        DESTINATION "${APP_NAME}/Contents/MacOS"
    )

    # Install CEF framework
    install(DIRECTORY "${CMAKE_BINARY_DIR}/Frameworks/Chromium Embedded Framework.framework"
        DESTINATION "${APP_NAME}/Contents/Frameworks"
    )

    # Symlink GL libraries next to executable (CEF GPU compositor needs them at executable path)
    install(CODE "
        execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
            \"../Frameworks/Chromium Embedded Framework.framework/Libraries/libEGL.dylib\"
            \"\${CMAKE_INSTALL_PREFIX}/${APP_NAME}/Contents/MacOS/libEGL.dylib\")
        execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink
            \"../Frameworks/Chromium Embedded Framework.framework/Libraries/libGLESv2.dylib\"
            \"\${CMAKE_INSTALL_PREFIX}/${APP_NAME}/Contents/MacOS/libGLESv2.dylib\")
    ")

    # Install Info.plist (configure version)
    if(EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
        file(READ "${CMAKE_SOURCE_DIR}/VERSION" APP_VERSION)
        string(STRIP "${APP_VERSION}" APP_VERSION)
    else()
        set(APP_VERSION "0.0.0")
    endif()
    configure_file(
        "${CMAKE_SOURCE_DIR}/resources/macos/Info.plist.in"
        "${CMAKE_BINARY_DIR}/Info.plist"
        @ONLY
    )
    install(FILES "${CMAKE_BINARY_DIR}/Info.plist"
        DESTINATION "${APP_NAME}/Contents"
    )

    # Install icon if exists
    if(EXISTS "${CMAKE_SOURCE_DIR}/resources/macos/AppIcon.icns")
        install(FILES "${CMAKE_SOURCE_DIR}/resources/macos/AppIcon.icns"
            DESTINATION "${APP_NAME}/Contents/Resources"
        )
    endif()

    # Create Resources directory (required even if empty)
    install(DIRECTORY DESTINATION "${APP_NAME}/Contents/Resources")

    # Install MoltenVK ICD manifest (Vulkan loader uses this to find bundled MoltenVK)
    install(FILES "${CMAKE_SOURCE_DIR}/resources/macos/MoltenVK_icd.json"
        DESTINATION "${APP_NAME}/Contents/Resources/vulkan/icd.d"
    )

    # Configure bundle completion script
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/CompleteBundleMac.cmake.in"
        "${CMAKE_BINARY_DIR}/CompleteBundleMac.cmake"
        @ONLY
    )

    # Run bundle completion (fixes library paths)
    install(SCRIPT "${CMAKE_BINARY_DIR}/CompleteBundleMac.cmake")
else()
    # Linux: CEF resources and libraries
    install(DIRECTORY "${CEF_RELEASE_DIR}/" DESTINATION .
        FILES_MATCHING PATTERN "*.so*" PATTERN "*.bin")
    install(DIRECTORY "${CEF_RESOURCE_DIR}/" DESTINATION .
        FILES_MATCHING PATTERN "*.pak" PATTERN "*.dat")
    install(DIRECTORY "${CEF_RESOURCE_DIR}/locales" DESTINATION .)
    if(EXTERNAL_MPV_DIR)
        install(FILES "${EXTERNAL_MPV_DIR}/lib/libmpv.so" DESTINATION .)
    endif()
endif()

# CPack for creating distribution packages
set(CPACK_PACKAGE_NAME "jellyfin-desktop")
set(CPACK_PACKAGE_VENDOR "Jellyfin")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Jellyfin Desktop Client")
file(READ "${CMAKE_SOURCE_DIR}/VERSION" CPACK_PACKAGE_VERSION)
string(STRIP "${CPACK_PACKAGE_VERSION}" CPACK_PACKAGE_VERSION)
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "TGZ")
endif()
include(CPack)
