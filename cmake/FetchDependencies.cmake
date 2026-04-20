include(FetchContent)
include(ExternalProject)

FetchContent_Declare(
    quill
    GIT_REPOSITORY https://github.com/odygrd/quill
    GIT_TAG master
)

FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest
    GIT_TAG master
)

FetchContent_MakeAvailable(quill)
FetchContent_MakeAvailable(doctest)

# Download and build libmpv
# HACK: Using ExternalProject_Add would be preferrable
# as you can also include the build and configure process
# through it. However, for Windows, CEF does not build properly
# using MSYS2 (which is used to build MPV). As a result,
# the windows workflow would need significant changes or a
# way to patch the compile arguments for Windows in
# third_party/cef/cmake/cef_variables.cmake to be compatible
# with MSYS or MINGW.
#
# For now, the project is fetched during configure time (but
# not built) so that the windows build can externally build it
# using the existing build_mpv_source.ps1 powershell script.
# 
# Alternatives:
# - Use ExternalProject_Add for non-windows machines. Example:
#     ExternalProject_Add(mpv
#         GIT_REPOSITORY https://github.com/andrewrabert/mpv/
#         GIT_TAG aa910a7f450ee7bd04423d36e9dba8af0327e90c
#         SOURCE_DIR "${MPV_SOURCE_DIR}"
#         CONFIGURE_COMMAND meson setup "${MPV_BUILD_DIR}" --default-library=shared -Dlibmpv=true #         BUILD_COMMAND meson compile -C "${MPV_BUILD_DIR}"
#         INSTALL_COMMAND ""
#         BUILD_BYPRODUCTS ${MPV_LIBRARY}
#         BUILD_IN_SOURCE true
#         STEP_TARGETS build)
#
# - Figure out how to build CEF using MSYS2 and create a patch file
#   to be applied in the workflow.
# - Build MPV using MSVC
FetchContent_Declare(mpv
    GIT_REPOSITORY https://github.com/andrewrabert/mpv/
    GIT_TAG aa910a7f450ee7bd04423d36e9dba8af0327e90c
)

FetchContent_GetProperties(mpv)
if(NOT mpv_POPULATED)
    FetchContent_Populate(mpv 
        GIT_REPOSITORY https://github.com/andrewrabert/mpv/
        GIT_TAG aa910a7f450ee7bd04423d36e9dba8af0327e90c
        SOURCE_DIR ${THIRD_PARTY_BASE_DIR}/mpv
        BINARY_DIR ${THIRD_PARTY_BASE_DIR}/mpv
    )
endif()

set(MPV_SOURCE_DIR ${mpv_SOURCE_DIR})
set(MPV_BUILD_DIR ${mpv_SOURCE_DIR}/build)
