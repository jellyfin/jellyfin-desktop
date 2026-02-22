
include(FetchDependencies)

if(WIN32)
  set(WINARCHSTR ARCHSTR windows-x86_64)
endif(WIN32)

# Derive QTROOT from CMAKE_PREFIX_PATH if not explicitly set (e.g. when Qt Creator
# configures the project via its Kit rather than a CMake preset).
if("${QTROOT}" STREQUAL "")
  foreach(_prefix ${CMAKE_PREFIX_PATH})
    if(EXISTS "${_prefix}/lib/cmake/Qt6/Qt6Config.cmake")
      set(QTROOT "${_prefix}")
      message(STATUS "QTROOT not set, derived from CMAKE_PREFIX_PATH: ${QTROOT}")
      break()
    endif()
  endforeach()
endif()

if((NOT IS_DIRECTORY ${QTROOT}) AND (NOT "${QTROOT}" STREQUAL ""))
  # Write qt.conf in the Qt depends directory so that the Qt tools can find QML files
  set(QTCONFCONTENT "[Paths]
    Prefix=${QTROOT}
    ")
  file(WRITE ${QTROOT}/bin/qt.conf ${QTCONFCONTENT})
endif()

message(STATUS "Qt root directory: ${QTROOT}")

list(APPEND CMAKE_FIND_ROOT_PATH ${QTROOT})
list(APPEND CMAKE_PREFIX_PATH ${QTROOT})
if(IS_DIRECTORY "${QTROOT}/include")
  include_directories(${QTROOT}/include)
endif()

set(REQUIRED_QT_VERSION "6.0.0")

set(QTCONFIGROOT ${QTROOT}/lib/cmake/Qt6)
set(components Core Network WebChannel Qml Quick Xml WebEngineQuick WebEngineCore Widgets OpenGL)

if(UNIX AND (NOT APPLE) AND ((NOT BUILD_TARGET STREQUAL "RPI")))
  set(components ${components} Gui)
endif()

if(LINUX_DBUS)
  set(components ${components} DBus)
endif(LINUX_DBUS)

foreach(COMP ${components})
	set(mod Qt6${COMP})

	# look for the config files in the QtConfigRoot defined above
	set(${mod}_DIR ${QTCONFIGROOT}${COMP})

	# look for the actual package
  find_package(Qt6 REQUIRED COMPONENTS ${COMP})

	include_directories(${${mod}_INCLUDE_DIRS})
	if(OPENELEC)
		include_directories(${${mod}_PRIVATE_INCLUDE_DIRS})
	endif(OPENELEC)

	# Need private interfaces for qpa/qplatformnativeinterface.h:
	if(${mod} STREQUAL Qt6Gui)
		include_directories(${Qt6Gui_PRIVATE_INCLUDE_DIRS})
	endif()

	list(APPEND QT6_LIBRARIES ${${mod}_LIBRARIES})
	list(APPEND QT6_CFLAGS ${${mod}_EXECUTABLE_COMPILE_FLAGS})
endforeach(COMP ${components})

if(QT6_CFLAGS)
	list(REMOVE_DUPLICATES QT6_CFLAGS)
  if(WIN32)
    list(REMOVE_ITEM QT6_CFLAGS -fPIC)
  endif(WIN32)
endif(QT6_CFLAGS)

message(STATUS "Qt version: ${Qt6Core_VERSION}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${QT6_CFLAGS}")

set(CMAKE_REQUIRED_INCLUDES ${Qt6WebEngineCore_INCLUDE_DIRS};${Qt6WebEngineCore_PRIVATE_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${QT6_LIBRARIES})

include(CheckCXXSourceCompiles)

CHECK_CXX_SOURCE_COMPILES(
"
  #include <QSurfaceFormat>

  int main(int argc, char** argv) {
    QSurfaceFormat::FormatOption o = QSurfaceFormat::UseOptimalOrientation;
    return 0;
  }
" QT6_HAVE_OPTIMALORIENTATION)

if(QT6_HAVE_OPTIMALORIENTATION)
  message(STATUS "QSurfaceFormat::UseOptimalOrientation found")
  add_definitions(-DHAVE_OPTIMALORIENTATION)
endif()
