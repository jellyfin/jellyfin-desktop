# Build mpv from submodule for Wayland subsurface rendering

set(MPV_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/mpv")
set(MPV_BUILD_DIR "${MPV_SOURCE_DIR}/build")

if(NOT EXISTS "${MPV_BUILD_DIR}/libmpv.so")
    message(STATUS "Building mpv from submodule...")
    execute_process(
        COMMAND meson setup build --default-library=shared -Dlibmpv=true
        WORKING_DIRECTORY ${MPV_SOURCE_DIR}
        RESULT_VARIABLE MPV_SETUP_RESULT
    )
    if(NOT MPV_SETUP_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to configure mpv with meson")
    endif()
    execute_process(
        COMMAND meson compile -C build
        WORKING_DIRECTORY ${MPV_SOURCE_DIR}
        RESULT_VARIABLE MPV_BUILD_RESULT
    )
    if(NOT MPV_BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to build mpv")
    endif()
endif()

set(MPV_INCLUDE_DIR "${MPV_SOURCE_DIR}/include")
set(MPV_LIBRARY "${MPV_BUILD_DIR}/libmpv.so")

# Set RPATH for finding bundled libmpv
set(CMAKE_BUILD_RPATH "${MPV_BUILD_DIR}")
set(CMAKE_INSTALL_RPATH "$ORIGIN/../lib")
