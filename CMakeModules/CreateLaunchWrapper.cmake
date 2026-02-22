# Post-build script: wrap the macOS binary in a launch script.
#
# Qt Creator's 'disclaim' tool converts _QTC_DYLD_FRAMEWORK_PATH into real
# DYLD_FRAMEWORK_PATH when launching a Mach-O binary.  This causes the wrong
# frameworks to load when Homebrew or other system frameworks are present.
#
# By making the main "binary" a shell script, disclaim applies to /bin/bash
# instead, and the real binary runs without any injected DYLD overrides.
# The app already has qt.conf and absolute framework paths, so DYLD_FRAMEWORK_PATH
# is not needed.
#
# Input variables: BINARY (path to the Mach-O binary just produced by the linker)

if(NOT EXISTS "${BINARY}")
  message(FATAL_ERROR "Binary not found: ${BINARY}")
endif()

get_filename_component(BIN_NAME "${BINARY}" NAME)
set(REAL_BINARY "${BINARY}.bin")

# The linker always writes a fresh Mach-O to BINARY, so rename it.
file(RENAME "${BINARY}" "${REAL_BINARY}")

# Create a thin wrapper that execs the real binary.
file(WRITE "${BINARY}"
"#!/bin/bash\nexec \"$(dirname \"$0\")/${BIN_NAME}.bin\" \"$@\"\n")

file(CHMOD "${BINARY}"
  PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
              GROUP_READ GROUP_EXECUTE
              WORLD_READ WORLD_EXECUTE)
