include_directories(
  ${CMAKE_SOURCE_DIR}/external/HIDRemote
  ${CMAKE_SOURCE_DIR}/external/letsmove
  ${CMAKE_SOURCE_DIR}/src/player
  core
  shared
)

set(CMAKE_AUTOMOC ON)
get_target_property(Qt6Core_RCC_EXECUTABLE Qt6::rcc IMPORTED_LOCATION)
add_definitions(-DPREFIX="${CMAKE_INSTALL_PREFIX}")

add_subdirectory(shared)
add_subdirectory(core)
add_subdirectory(display)
add_subdirectory(player)
add_subdirectory(utils)
add_subdirectory(ui)
add_subdirectory(input)
add_subdirectory(system)
add_subdirectory(settings)
add_subdirectory(power)
add_subdirectory(taskbar)

if(LINUX_DBUS)
  add_subdirectory(mpris)
endif()

get_property(ALL_SRCS GLOBAL PROPERTY SRCS_LIST)
set(MAIN_SRCS main.cpp)

source_group("Source Files" FILES ${MAIN_SRCS})

# Set some Objective-C flags.
# We need to force the Language to C instead of C++
# and also make sure that we use ARC
#
foreach(S ${ALL_SRCS})
  string(REGEX MATCH ".*\\.m$" MATCH_OBJC ${S})
  if(MATCH_OBJC)
    set_property(SOURCE ${S} PROPERTY COMPILE_FLAGS "-fobjc-arc")
    set_property(SOURCE ${S} PROPERTY LANGUAGE C)
  endif()
endforeach(S SOURCE)

foreach(sfile in ${ALL_SRCS})
  get_filename_component(REALNAME ${sfile} REALPATH)
  get_filename_component(DIRNAME ${REALNAME} DIRECTORY)
  string(REPLACE "${CMAKE_SOURCE_DIR}/src/" "" SUBDIR ${DIRNAME})
  string(TOLOWER ${SUBDIR} SUBDIR)
  string(REPLACE "/" "\\\\" GNAME ${SUBDIR})
  source_group("Source Files\\\\${GNAME}" FILES ${sfile})
endforeach(sfile in ${ALL_SRCS})

# Build core sources as static library for reuse by tests
add_library(jmp_core STATIC ${ALL_SRCS})

file(GLOB_RECURSE RESOURCE_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/resources/*)
file(GLOB_RECURSE NATIVE_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/native/*)

# Generate resources - runs rcc, only writes output if changed
# Note: webview.qml is handled separately via qt_add_qml_module for AOT compilation
add_custom_target(generate_resources ALL
  COMMAND ${CMAKE_COMMAND}
          -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/qrc_resources.cpp
          -DRCC=${Qt6Core_RCC_EXECUTABLE}
          -DRESOURCES_DIR=${CMAKE_SOURCE_DIR}/resources
          -DNATIVE_DIR=${CMAKE_SOURCE_DIR}/native
          -P ${CMAKE_SOURCE_DIR}/CMakeModules/GenerateQtResources.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/qrc_resources.cpp
  VERBATIM
  COMMENT "Generating resources"
)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/qrc_resources.cpp PROPERTIES GENERATED TRUE)

list(APPEND RESOURCE_FILES ${CMAKE_CURRENT_BINARY_DIR}/qrc_resources.cpp)

set(MACOSX_BUNDLE_ICON_FILE jellyfin.icns)
set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${VERSION_STRING})
set(MACOSX_BUNDLE_BUNDLE_NAME "Jellyfin")
set(MACOSX_BUNDLE_BUNDLE_VERSION ${VERSION_STRING})
set(MACOSX_BUNDLE_GUI_IDENTIFIER org.jellyfin.JellyfinDesktop)

if(APPLE)
  if(HAVE_UPDATER)
    add_resources(TARGET ${MAIN_TARGET} SOURCES ${UPDATER_PATH} DEST ${INSTALL_RESOURCE_DIR})
  endif(HAVE_UPDATER)

  set_source_files_properties(
    input/apple/HIDRemote/HIDRemote.m
    PROPERTIES COMPILE_FLAGS -fno-objc-arc
  )

  set(ICON_FILE ${CMAKE_SOURCE_DIR}/bundle/osx/${MACOSX_BUNDLE_ICON_FILE})
  list(APPEND RESOURCE_FILES ${ICON_FILE})
  set_source_files_properties(${ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

if(WIN32)
  list(APPEND RESOURCE_FILES ${CMAKE_SOURCE_DIR}/bundle/win/iconres.rc)
endif()

# Generate qt.conf so that dev builds find Qt plugins and QML files
# without needing DYLD_FRAMEWORK_PATH / QT_PLUGIN_PATH / PATH env vars.
if(NOT "${QTROOT}" STREQUAL "")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/qt.conf "[Paths]\nPrefix=${QTROOT}\n")
  add_resources(TARGET ${MAIN_TARGET} SOURCES ${CMAKE_CURRENT_BINARY_DIR}/qt.conf DEST ${INSTALL_RESOURCE_DIR})
endif()

set(RESOURCE_ROOT .)
if(NOT APPLE)
  set(rsrc_locations
    ${QTROOT}/resources
    /usr/share/qt/resources
  )
  foreach(loc ${rsrc_locations})
    if(EXISTS ${loc}/qtwebengine_devtools_resources.pak)
      install(FILES ${loc}/qtwebengine_devtools_resources.pak DESTINATION resources)
    endif()
  endforeach()
endif()

if(XCODE)
  get_resources_source_list(${MAIN_TARGET} XCODE_RESOURCES)
endif()

get_property(BUNDLED_FILES GLOBAL PROPERTY CONFIG_BUNDLED_FILES)
add_executable(${MAIN_TARGET} WIN32 MACOSX_BUNDLE ${MAIN_SRCS} ${BUNDLED_FILES} ${RESOURCE_FILES} ${XCODE_RESOURCES})
add_dependencies(${MAIN_TARGET} generate_resources)
std_target_properties(${MAIN_TARGET})
if(APPLE)
  set(_install_rpath "@executable_path/../Frameworks;${QTROOT}/lib")
else()
  set(_install_rpath "${QTROOT}/lib")
endif()
set_target_properties(${MAIN_TARGET} PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/bundle/osx/Info.plist.in
  INSTALL_RPATH "${_install_rpath}"
  OUTPUT_NAME ${MAIN_NAME}
)
copy_resources(${MAIN_TARGET})
clang_tidy(${MAIN_TARGET})

# Workaround for Qt Creator locating incorrect Qt version via DYLD_FRAMEWORK_PATH.
# This forces the build to load qt.conf's paths instead, which should be correct.
if(APPLE AND MACOS_LAUNCH_WRAPPER)
  add_custom_command(TARGET ${MAIN_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -DBINARY=$<TARGET_FILE:${MAIN_TARGET}>
      -P ${CMAKE_SOURCE_DIR}/CMakeModules/MacOSLaunchWrapper.cmake
    COMMENT "Creating macOS launch wrapper"
  )
endif()

# QML module for AOT compilation (eliminates runtime qmlcache)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/ui/webview.qml
  PROPERTIES QT_RESOURCE_ALIAS webview.qml
)
qt_add_qml_module(${MAIN_TARGET}
  URI Konvergo
  VERSION 1.0
  QML_FILES ${CMAKE_CURRENT_SOURCE_DIR}/ui/webview.qml
  NO_RESOURCE_TARGET_PATH
)

find_library(MINIZIP_LIBRARY minizip)
if(WIN32)
  # FindZLIB doesn't find this. (It might be possible to fix the minizip build
  # to not require this.)
  find_library(ZLIB_LIBRARIES zlibwapi)
  if(ZLIB_LIBRARIES)
    set(ZLIB_FOUND on)
  endif()
else()
  include(FindZLIB)
endif()

if(ZLIB_FOUND AND MINIZIP_LIBRARY)
  message(STATUS "Using minizip")
  set(MINIZIP_LIBS ${MINIZIP_LIBRARY} ${ZLIB_LIBRARIES})
  add_definitions(-DHAVE_MINIZIP)
else()
  if(ENABLE_CODECS)
    if (ZLIB_FOUND)
      message(FATAL_ERROR "minizip required in this configuration")
    else()
      message(FATAL_ERROR "zlib required in this configuration")
    endif()
  endif()
endif()

if(USE_STATIC_MPVQT)
  set(MPVQT_TARGET MpvQt)
else()
  set(MPVQT_TARGET MpvQt::MpvQt)
endif()

# Link core library with all dependencies
target_link_libraries(jmp_core
  shared
  ${MPV_LIBRARY}
  ${OPENGL_LIBS}
  ${QT6_LIBRARIES}
  ${OS_LIBS}
  ${EXTRA_LIBS}
  ${X11_LIBRARIES}
  ${X11_Xrandr_LIB}
  ${CMAKE_THREAD_LIBS_INIT}
  ${RPI_LIBS}
  ${MINIZIP_LIBS}
  Qt6::Core
  Qt6::Quick
  Qt6::Widgets
  Qt6::WebEngineQuick
  Qt6::WebEngineCore
  Qt6::OpenGL
  Qt6::Xml
  Qt6::Gui
  ${MPVQT_TARGET}
)

if(LINUX_DBUS)
  target_link_libraries(jmp_core Qt6::DBus)
endif()

# Delay-load libmpv on Windows for runtime AVX2 fallback
if(WIN32)
  target_link_libraries(jmp_core delayimp.lib)
  target_link_options(jmp_core PRIVATE /DELAYLOAD:libmpv-2.dll)
endif()

target_link_libraries(${MAIN_TARGET} PRIVATE jmp_core)
install(TARGETS ${MAIN_TARGET} DESTINATION ${INSTALL_BIN_DIR})

set(EXE "${MAIN_NAME}.app")
set(LIBPATH ${CMAKE_FIND_ROOT_PATH})
set(SOURCE_ROOT ${CMAKE_SOURCE_DIR})

include(CompleteBundle)
