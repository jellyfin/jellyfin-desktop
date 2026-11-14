include(GNUInstallDirs)

find_package(X11)
if(X11_FOUND AND X11_Xrandr_FOUND)
  include_directories(${X11_X11_INCLUDE_PATH} ${X11_Xrandr_INCLUDE_PATH})
  set(X11XRANDR_FOUND 1)
  add_definitions(-DUSE_X11XRANDR)
else()
  set(X11_LIBRARIES "")
  set(X11_Xrandr_LIB "")
endif()

# Display mode switching on KDE Plasma Wayland sessions
find_package(Qt6 COMPONENTS WaylandClient QUIET)
find_path(PLASMA_WAYLAND_PROTOCOLS_DIR kde-output-device-v2.xml
  PATHS ${CMAKE_INSTALL_FULL_DATADIR} /usr/share
  PATH_SUFFIXES plasma-wayland-protocols)
# kde_output_device_registry_v2 needs plasma-wayland-protocols 1.21 or newer
if(PLASMA_WAYLAND_PROTOCOLS_DIR)
  file(STRINGS ${PLASMA_WAYLAND_PROTOCOLS_DIR}/kde-output-device-v2.xml HAS_OUTPUT_REGISTRY
    REGEX "kde_output_device_registry_v2")
endif()
if(Qt6WaylandClient_FOUND AND HAS_OUTPUT_REGISTRY)
  set(KDEWAYLAND_FOUND 1)
  add_definitions(-DUSE_KDE_WAYLAND)
  message(STATUS "Enabling KDE Wayland display mode switching")
endif()

if(LINUX_X11POWER)
  add_definitions(-DUSE_X11POWER)
  Message(STATUS "Enabling X11/XDG screensaver management")
else()
  add_definitions(-DLINUX_DBUS=1)
  Message(STATUS "Enabling D-Bus power management")
endif()

set(INSTALL_BIN_DIR ${CMAKE_INSTALL_BINDIR})
set(INSTALL_RESOURCE_DIR ${CMAKE_INSTALL_DATADIR}/jellyfin-desktop)

if(NOT OPENELEC)
  include(InstallLinuxDesktopFile)
endif()
