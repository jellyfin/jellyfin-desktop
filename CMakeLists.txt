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
        message(FATAL_ERROR
            "CEF version mismatch:\n"
            "  CEF_VERSION file: ${_EXPECTED_CEF_VERSION}\n"
            "  Found CEF:        ${CEF_VERSION}")
    endif()
endif()

# SDL3
find_package(SDL3 REQUIRED)

# Vulkan (for video subsurface)
# On Windows ARM64, hint the correct Vulkan library path
if(WIN32 AND CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64" AND DEFINED ENV{VULKAN_SDK})
    set(Vulkan_LIBRARY "$ENV{VULKAN_SDK}/Lib/vulkan-1.lib")
endif()
find_package(Vulkan REQUIRED)

# Platform-specific dependencies
if(APPLE)
    # macOS: OpenGL via CGL, Metal frameworks
    find_package(OpenGL REQUIRED)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
    find_library(MEDIAPLAYER_FRAMEWORK MediaPlayer REQUIRED)

    set(PLATFORM_SOURCES
        src/platform/macos_layer.mm
        src/platform/macos_app.mm
        src/compositor/metal_compositor.mm
        src/player/media_session.cpp
        src/player/macos/media_session_macos.mm
        src/player/mpv/mpv_player.cpp
        src/player/vulkan_subsurface_renderer.cpp
    )

    # LetsMove - prompts user to move app to /Applications (clears quarantine)
    add_subdirectory(third_party/letsmove)
    set(PLATFORM_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/letsmove)

    set(PLATFORM_LIBRARIES
        ${COCOA_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${IOSURFACE_FRAMEWORK}
        ${MEDIAPLAYER_FRAMEWORK}
        letsmove
    )
elseif(WIN32)
    # Windows: WGL for CEF compositor, Vulkan for video (gpu-next via DComp)
    find_package(OpenGL REQUIRED)

    set(PLATFORM_SOURCES
        src/context/wgl_context.cpp
        src/context/gl_loader.cpp
        src/context/opengl_frame_context.cpp
        src/platform/windows_video_surface.cpp
        src/platform/windows_overlay_layer.cpp
        src/platform/dcomp_browser_layer.cpp
        src/compositor/opengl_compositor.cpp
        src/player/media_session.cpp
        src/player/mpv/mpv_player.cpp
        src/player/vulkan_subsurface_renderer.cpp
        src/player/windows/media_session_windows.cpp
        ${CMAKE_SOURCE_DIR}/resources/win/iconres.rc
    )
    set(PLATFORM_LIBRARIES
        OpenGL::GL
        dwmapi
        d3d11
        dxgi
        dcomp
        crypt32
        windowsapp
    )
    set(PLATFORM_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party")
else()
    # Linux: OpenGL/EGL, Wayland, X11
    find_package(OpenGL REQUIRED COMPONENTS EGL OpenGL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_EGL REQUIRED wayland-egl)
    pkg_check_modules(LIBDRM REQUIRED libdrm)
    pkg_check_modules(WAYLAND REQUIRED wayland-client)
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
    pkg_check_modules(LIBPLACEBO REQUIRED libplacebo)
    pkg_check_modules(SYSTEMD REQUIRED libsystemd)
    pkg_check_modules(X11 REQUIRED x11)

    # Find wayland-scanner
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

    # Generate Wayland protocol sources
    set(WAYLAND_GENERATED_DIR "${CMAKE_BINARY_DIR}/wayland-protocols")
    file(MAKE_DIRECTORY ${WAYLAND_GENERATED_DIR})

    set(COLOR_MGMT_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/staging/color-management/color-management-v1.xml")
    set(COLOR_MGMT_CLIENT_H "${WAYLAND_GENERATED_DIR}/color-management-v1-client.h")
    set(COLOR_MGMT_CODE_C "${WAYLAND_GENERATED_DIR}/color-management-v1-code.c")

    set(VIEWPORTER_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml")
    set(VIEWPORTER_CLIENT_H "${WAYLAND_GENERATED_DIR}/viewporter-client.h")
    set(VIEWPORTER_CODE_C "${WAYLAND_GENERATED_DIR}/viewporter-code.c")

    set(XDG_ACTIVATION_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/staging/xdg-activation/xdg-activation-v1.xml")
    set(XDG_ACTIVATION_CLIENT_H "${WAYLAND_GENERATED_DIR}/xdg-activation-v1-client.h")
    set(XDG_ACTIVATION_CODE_C "${WAYLAND_GENERATED_DIR}/xdg-activation-v1-code.c")

    # KDE server decoration palette — per-window titlebar colors on KWin.
    # Required by default; pass -DENABLE_KDE_PALETTE=OFF to disable.
    option(ENABLE_KDE_PALETTE "KDE/KWin per-window titlebar color support (requires plasma-wayland-protocols at build time)" ON)
    if(ENABLE_KDE_PALETTE)
        find_file(DECO_PALETTE_PROTOCOL server-decoration-palette.xml
            PATHS /usr/share/plasma-wayland-protocols
                  /app/share/plasma-wayland-protocols
            REQUIRED)
        set(DECO_PALETTE_CLIENT_H "${WAYLAND_GENERATED_DIR}/server-decoration-palette-client.h")
        set(DECO_PALETTE_CODE_C "${WAYLAND_GENERATED_DIR}/server-decoration-palette-code.c")
    endif()

    add_custom_command(
        OUTPUT ${COLOR_MGMT_CLIENT_H}
        COMMAND ${WAYLAND_SCANNER} client-header ${COLOR_MGMT_PROTOCOL} ${COLOR_MGMT_CLIENT_H}
        DEPENDS ${COLOR_MGMT_PROTOCOL}
        COMMENT "Generating color-management-v1 client header"
    )
    add_custom_command(
        OUTPUT ${COLOR_MGMT_CODE_C}
        COMMAND ${WAYLAND_SCANNER} private-code ${COLOR_MGMT_PROTOCOL} ${COLOR_MGMT_CODE_C}
        DEPENDS ${COLOR_MGMT_PROTOCOL}
        COMMENT "Generating color-management-v1 code"
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
        OUTPUT ${XDG_ACTIVATION_CLIENT_H}
        COMMAND ${WAYLAND_SCANNER} client-header ${XDG_ACTIVATION_PROTOCOL} ${XDG_ACTIVATION_CLIENT_H}
        DEPENDS ${XDG_ACTIVATION_PROTOCOL}
        COMMENT "Generating xdg-activation-v1 client header"
    )
    add_custom_command(
        OUTPUT ${XDG_ACTIVATION_CODE_C}
        COMMAND ${WAYLAND_SCANNER} private-code ${XDG_ACTIVATION_PROTOCOL} ${XDG_ACTIVATION_CODE_C}
        DEPENDS ${XDG_ACTIVATION_PROTOCOL}
        COMMENT "Generating xdg-activation-v1 code"
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
        src/context/egl_context.cpp
        src/context/opengl_frame_context.cpp
        src/platform/wayland_subsurface.cpp
        src/platform/x11_video_layer.cpp
        src/platform/event_loop_linux.cpp
        src/compositor/opengl_compositor.cpp
        src/player/media_session.cpp
        src/player/mpris/media_session_mpris.cpp
        src/player/mpv/mpv_player.cpp
        src/player/vulkan_subsurface_renderer.cpp
        src/player/opengl_renderer.cpp
        ${COLOR_MGMT_CODE_C}
        ${COLOR_MGMT_CLIENT_H}
        ${VIEWPORTER_CODE_C}
        ${VIEWPORTER_CLIENT_H}
        ${XDG_ACTIVATION_CODE_C}
        ${XDG_ACTIVATION_CLIENT_H}
    )
    if(ENABLE_KDE_PALETTE)
        list(APPEND PLATFORM_SOURCES
            src/platform/kde_decoration_palette.cpp
            ${DECO_PALETTE_CODE_C}
            ${DECO_PALETTE_CLIENT_H}
        )
        add_compile_definitions(HAVE_KDE_DECORATION_PALETTE)
    endif()
    set(PLATFORM_LIBRARIES
        ${WAYLAND_LIBRARIES}
        ${WAYLAND_EGL_LIBRARIES}
        ${LIBPLACEBO_LIBRARIES}
        ${SYSTEMD_LIBRARIES}
        ${X11_LIBRARIES}
        OpenGL::EGL
        OpenGL::OpenGL
        ${CMAKE_DL_LIBS}
    )
    set(PLATFORM_INCLUDE_DIRS
        ${WAYLAND_INCLUDE_DIRS}
        ${WAYLAND_EGL_INCLUDE_DIRS}
        ${LIBPLACEBO_INCLUDE_DIRS}
        ${LIBDRM_INCLUDE_DIRS}
        ${SYSTEMD_INCLUDE_DIRS}
        ${X11_INCLUDE_DIRS}
        ${CMAKE_BINARY_DIR}
    )
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

# Common sources for all platforms
set(COMMON_SOURCES
    src/main.cpp
    src/logging.cpp
    src/single_instance.cpp
    src/window_activation.cpp
    src/browser/browser_stack.cpp
    src/cef/cef_app.cpp
    src/cef/cef_client.cpp
    src/cef/cef_thread.cpp
    src/cef/resource_handler.cpp
    src/context/vulkan_context.cpp
    src/player/video_stack.cpp
    src/player/mpv_event_thread.cpp
    src/player/video_render_controller.cpp
    src/player/media_session_thread.cpp
    src/cjson/cJSON.c
    src/settings.cpp
    src/window_geometry.cpp
    src/ui/menu_overlay.cpp
)

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
    ${Vulkan_INCLUDE_DIRS}
    ${PLATFORM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(jellyfin-desktop PRIVATE
    ${CEF_LIBRARIES}
    SDL3::SDL3
    ${MPV_LIBRARIES}
    Vulkan::Vulkan
    ${PLATFORM_LIBRARIES}
)

# When using external CEF, tell the app where to find resources
if(EXTERNAL_CEF_DIR)
    target_compile_definitions(jellyfin-desktop PRIVATE
        CEF_RESOURCES_DIR="${CEF_RESOURCE_DIR}"
    )
endif()

# Enable ARC for Objective-C++ files on macOS
if(APPLE)
    set_source_files_properties(
        src/platform/macos_layer.mm
        src/platform/macos_app.mm
        src/compositor/metal_compositor.mm
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
    if(EXTERNAL_CEF_DIR)
        list(APPEND RPATH_DIRS "${EXTERNAL_CEF_DIR}/lib")
    endif()
    list(JOIN RPATH_DIRS ";" RPATH_STRING)
    set_target_properties(jellyfin-desktop PROPERTIES
        INSTALL_RPATH "${RPATH_STRING}"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# Copy CEF resources to build directory (only when not using external CEF)
if(NOT EXTERNAL_CEF_DIR)
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

    # SDL3
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64")
        install(FILES "${CMAKE_SOURCE_DIR}/third_party/SDL/lib/arm64/SDL3.dll" DESTINATION .)
    else()
        install(FILES "${CMAKE_SOURCE_DIR}/third_party/SDL/lib/x64/SDL3.dll" DESTINATION .)
    endif()

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
