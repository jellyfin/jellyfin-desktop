if(APPLE)
  add_subdirectory(osx)
elseif(BUILD_TARGET STREQUAL "RPI")
  add_subdirectory(rpi)
elseif(X11XRANDR_FOUND)
  add_subdirectory(x11)
elseif(WIN32)
  add_subdirectory(win)
endif()

if(KDEWAYLAND_FOUND)
  add_subdirectory(kde)
endif()

add_subdirectory(dummy)

find_all_sources(. SRC)
add_sources(${SRC})