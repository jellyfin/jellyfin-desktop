# Post-build script for Qt Creator on MacOS
#
# Qt Creator overwrites the DYLD_FRAMEWORK_PATH when launching a Mac binary.  This 
# may cause the wrong version of Qt to load when more than one version of Qt is present,
# for example the Homebrew version.
#
# This "one weird trick" fixes the issue by tricking Qt Creator into thinking we're
# loading a shell script instead of a Mac binary to prevent the path madness.
#
# Input variables: BINARY (path to the Mac binary just produced by the linker)

if(NOT EXISTS "${BINARY}")
  message(FATAL_ERROR "Binary not found: ${BINARY}")
endif()

get_filename_component(BIN_NAME "${BINARY}" NAME)
set(REAL_BINARY "${BINARY}.bin")

file(RENAME "${BINARY}" "${REAL_BINARY}")

# Create a thin wrapper that execs the real binary.
file(WRITE "${BINARY}"
"#!/bin/bash\nexec \"$(dirname \"$0\")/${BIN_NAME}.bin\" \"$@\"\n")

file(CHMOD "${BINARY}"
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
              GROUP_READ GROUP_EXECUTE
              WORLD_READ WORLD_EXECUTE)
