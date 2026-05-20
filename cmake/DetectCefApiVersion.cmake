# DetectCefApiVersion.cmake — derive CEF_API_VERSION from the cef-dll-sys crate.
#
# The Rust binding crate (cef-dll-sys, pulled in via the `cef` crate) ships
# pre-generated bindings whose CEF_API_VERSION_LAST constant defines the ABI
# the Rust handler structs were generated against. C++ code and the
# libcef_dll_wrapper must compile against that exact same value or runtime
# CHECK(base.size) aborts on the first Rust→C++ handoff:
#
#   FATAL:cef_wrapper/libcef_dll/ctocpp/ctocpp_ref_counted.h:124] Cannot wrap
#   struct with invalid base.size value (got 584, expected 592) at API version
#   999999
#
# Single source of truth: the crate itself. Bumping the `cef` dep in
# Cargo.toml automatically updates this value the next time CMake configures.

set(_WORKSPACE_MANIFEST "${CMAKE_SOURCE_DIR}/src/Cargo.toml")

find_program(CARGO_EXECUTABLE cargo REQUIRED)

execute_process(
    COMMAND ${CARGO_EXECUTABLE} metadata --format-version 1
        --manifest-path "${_WORKSPACE_MANIFEST}"
    OUTPUT_VARIABLE _CARGO_METADATA
    RESULT_VARIABLE _CARGO_METADATA_RESULT
    ERROR_VARIABLE _CARGO_METADATA_STDERR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _CARGO_METADATA_RESULT EQUAL 0)
    message(FATAL_ERROR
        "cargo metadata failed (rc=${_CARGO_METADATA_RESULT}):\n${_CARGO_METADATA_STDERR}")
endif()

string(JSON _PKG_COUNT LENGTH "${_CARGO_METADATA}" packages)
math(EXPR _PKG_LAST "${_PKG_COUNT} - 1")
set(_CEF_SYS_MANIFEST "")
foreach(_i RANGE 0 ${_PKG_LAST})
    string(JSON _NAME GET "${_CARGO_METADATA}" packages ${_i} name)
    if(_NAME STREQUAL "cef-dll-sys")
        string(JSON _CEF_SYS_MANIFEST GET "${_CARGO_METADATA}" packages ${_i} manifest_path)
        break()
    endif()
endforeach()
if(NOT _CEF_SYS_MANIFEST)
    message(FATAL_ERROR
        "cef-dll-sys not found in cargo metadata. The `cef` dependency in "
        "src/jfn_cef/Cargo.toml is expected to pull it in transitively.")
endif()

get_filename_component(_CEF_SYS_DIR "${_CEF_SYS_MANIFEST}" DIRECTORY)
file(GLOB _CEF_SYS_BINDINGS "${_CEF_SYS_DIR}/src/bindings/*_*.rs")
if(NOT _CEF_SYS_BINDINGS)
    message(FATAL_ERROR
        "No binding files under ${_CEF_SYS_DIR}/src/bindings/. "
        "cef-dll-sys layout may have changed; update DetectCefApiVersion.cmake.")
endif()
list(GET _CEF_SYS_BINDINGS 0 _CEF_SYS_BINDING)
file(READ "${_CEF_SYS_BINDING}" _BINDING_CONTENT)

if(NOT _BINDING_CONTENT MATCHES "pub const CEF_API_VERSION_LAST: i32 = ([0-9]+);")
    message(FATAL_ERROR
        "CEF_API_VERSION_LAST not found in ${_CEF_SYS_BINDING}. "
        "cef-dll-sys layout may have changed.")
endif()
set(JFN_CEF_API_VERSION "${CMAKE_MATCH_1}" CACHE INTERNAL
    "CEF API version pinned by cef-dll-sys (auto-detected)")

message(STATUS "Detected CEF_API_VERSION=${JFN_CEF_API_VERSION} from ${_CEF_SYS_MANIFEST}")
