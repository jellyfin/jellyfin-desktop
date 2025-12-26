
add_sources(
  CachedRegexMatcher.cpp CachedRegexMatcher.h
  Utils.cpp Utils.h
  Log.cpp Log.h
  PlatformInfo.cpp PlatformInfo.h
)

if(APPLE)
  add_subdirectory(osx)
endif(APPLE)

if(WIN32)
  add_subdirectory(win)
endif()
