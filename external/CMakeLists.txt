set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMPILER_FLAGS_THIRD_PARTY}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMPILER_FLAGS_THIRD_PARTY}")

if(APPLE)
  add_subdirectory(plistparser)
  add_subdirectory(HIDRemote)
  add_subdirectory(letsmove)
endif(APPLE)

if(USE_STATIC_MPVQT)
  # Build MpvQt directly from sources
  add_library(MpvQt STATIC
    mpvqt/src/mpvabstractitem.cpp
    mpvqt/src/mpvcontroller.cpp
    mpvrenderer-wrapper.cpp
  )
  target_include_directories(MpvQt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/mpvqt/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/MpvQt
  )
  target_link_libraries(MpvQt PUBLIC
    Qt6::Quick
    ${MPV_LIBRARY}
  )
  set_target_properties(MpvQt PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    AUTOMOC ON
  )
  # Create Qt-style forwarding headers
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/MpvQt/MpvAbstractItem "#include \"mpvabstractitem.h\"\n")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/MpvQt/MpvController "#include \"mpvcontroller.h\"\n")
endif()

# Build mpv from submodule using meson
set(MPV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mpv")
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

