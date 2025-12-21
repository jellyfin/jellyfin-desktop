if(USE_WAYLAND_SUBSURFACE)
  message(STATUS "Using Wayland subsurface for mpv rendering (HDR enabled)")
  find_package(Vulkan REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
  find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)

  include(WaylandMpvConfiguration)
  include_directories(${MPV_INCLUDE_DIR})
  include_directories(${WAYLAND_CLIENT_INCLUDE_DIRS})

  add_compile_definitions(USE_WAYLAND_SUBSURFACE)

  set(WAYLAND_MPV_SOURCES
    ${CMAKE_SOURCE_DIR}/src/player/WaylandVulkanContext.cpp
    ${CMAKE_SOURCE_DIR}/src/player/WaylandMpvPlayer.cpp
  )
  set(WAYLAND_MPV_LIBS
    ${MPV_LIBRARY}
    Vulkan::Vulkan
    ${WAYLAND_CLIENT_LIBRARIES}
    Qt6::GuiPrivate
  )
else()
  # We want OpenGL or OpenGLES2
  find_package(OpenGL)
  if(NOT OPENGL_FOUND)
    find_package(GLES2)
    if(NOT GLES2_FOUND)
      message(FATAL_ERROR "OpenGL or GLES2 is required")
    else(NOT GLES2_FOUND)
      set(OPENGL_LIBS ${GLES2_LIBRARY})
    endif(NOT GLES2_FOUND)
  else(NOT OPENGL_FOUND)
    set(OPENGL_LIBS ${OPENGL_gl_LIBRARY})
  endif(NOT OPENGL_FOUND)

  find_package(MPV REQUIRED)
  include_directories(${MPV_INCLUDE_DIR})

  set(WAYLAND_MPV_SOURCES "")
  set(WAYLAND_MPV_LIBS "")
endif()
