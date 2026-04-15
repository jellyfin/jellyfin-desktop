# 2012-01-31, Lars Bilke
# - Enable Code Coverage
#
# 2013-09-17, Joakim Söderberg
# - Added support for Clang.
# - Some additional usage instructions.
#
# 2018-03-31, Bendik Samseth
# - Relax debug output.
# - Keep a copy of the coverage output for later use.
# - Updated coverage exclude patterns.
#
# 2018-01-03, HenryRLee
# - Allow for *Clang compiler names, not just Clang.
#
# 2018-01-03, Bendik Samseth
# - Only check compiler compatibility if in a coverage build.
#
#
# USAGE:

# 0. (Mac only) If you use Xcode 5.1 make sure to patch geninfo as described here:
#      http://stackoverflow.com/a/22404544/80480
#
# 1. Copy this file into your cmake modules path.
#
# 2. Add the following line to your CMakeLists.txt:
#      INCLUDE(CodeCoverage)
#
# 3. Set compiler flags to turn off optimization and enable coverage:
#    SET(CMAKE_CXX_FLAGS "-g -O0 -fprofile-arcs -ftest-coverage")
#	 SET(CMAKE_C_FLAGS "-g -O0 -fprofile-arcs -ftest-coverage")
#
# 3. Use the function SETUP_TARGET_FOR_COVERAGE to create a custom make target
#    which runs your test executable and produces a lcov code coverage report:
#    Example:
#	 SETUP_TARGET_FOR_COVERAGE(
#				my_coverage_target  # Name for custom target.
#				test_driver         # Name of the test driver executable that runs the tests.
#									# NOTE! This should always have a ZERO as exit code
#									# otherwise the coverage generation will not complete.
#				coverage            # Name of output directory.
#				)
#
# 4. Build a Debug build:
#	 cmake -DCMAKE_BUILD_TYPE=Debug ..
#	 make
#	 make my_coverage_target
#
#


# Param _targetname     The name of new the custom make target
# Param _testrunner     The name of the target which runs the tests.
#						MUST return ZERO always, even on errors.
#						If not, no coverage report will be created!
# Param _outputname     lcov output is generated as _outputname.info
#                       HTML report is generated in _outputname/index.html
# Optional fourth parameter is passed as arguments to _testrunner
#   Pass them in list form, e.g.: "-j;2" for -j 2
FUNCTION(SETUP_TARGET_FOR_COVERAGE _targetname _testrunner _outputname)

    IF (NOT LCOV_PATH)
        MESSAGE(FATAL_ERROR "lcov not found! Aborting...")
    ENDIF () # NOT LCOV_PATH

    IF (NOT GENHTML_PATH)
        MESSAGE(FATAL_ERROR "genhtml not found! Aborting...")
    ENDIF () # NOT GENHTML_PATH

    # Setup target
    ADD_CUSTOM_TARGET(${_targetname}

            # Cleanup lcov
            ${LCOV_PATH} --directory . --zerocounters

            # Run tests
            COMMAND ${_testrunner} ${ARGV3}

            # Capturing lcov counters and generating report
            COMMAND ${LCOV_PATH} --directory . --capture --output-file ${_outputname}.info

            COMMAND ${LCOV_PATH} --remove ${_outputname}.info '*/tests/*' '/usr/*' '*/external/*' '/Applications/*' --output-file ${_outputname}.info.cleaned
            COMMAND ${GENHTML_PATH} -o ${_outputname} ${_outputname}.info.cleaned
            COMMAND ${LCOV_PATH} --list ${_outputname}.info.cleaned

            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Resetting code coverage counters to zero.\nProcessing code coverage counters and generating report."
            )

    # Show info where to find the report
    ADD_CUSTOM_COMMAND(TARGET ${_targetname} POST_BUILD
            COMMAND ;
            COMMENT "${BoldMagenta}Open ./${_outputname}/index.html in your browser to view the coverage report.${ColourReset}"
            )

ENDFUNCTION() # SETUP_TARGET_FOR_COVERAGE


string(TOLOWER "${CMAKE_BUILD_TYPE}" cmake_build_type_tolower)
if (cmake_build_type_tolower STREQUAL "coverage")


    # Check prereqs
    FIND_PROGRAM(GCOV_PATH gcov)
    FIND_PROGRAM(LCOV_PATH lcov)
    FIND_PROGRAM(GENHTML_PATH genhtml)
    FIND_PROGRAM(GCOVR_PATH gcovr PATHS ${CMAKE_SOURCE_DIR}/tests)

    IF (NOT GCOV_PATH)
        MESSAGE(FATAL_ERROR "gcov not found! Aborting...")
    ENDIF () # NOT GCOV_PATH

    IF (NOT CMAKE_COMPILER_IS_GNUCXX)
        IF (NOT "${CMAKE_CXX_COMPILER_ID}" MATCHES ".*Clang")
            MESSAGE(FATAL_ERROR "Compiler is not GNU gcc or Clang! Aborting...")
        ENDIF ()
    ENDIF () # NOT CMAKE_COMPILER_IS_GNUCXX

    SET(CMAKE_CXX_FLAGS_COVERAGE
            "-g -O0 -fprofile-arcs -ftest-coverage"
            CACHE STRING "Flags used by the C++ compiler during coverage builds."
            FORCE)
    SET(CMAKE_C_FLAGS_COVERAGE
            "-g -O0 -fprofile-arcs -ftest-coverage"
            CACHE STRING "Flags used by the C compiler during coverage builds."
            FORCE)
    SET(CMAKE_EXE_LINKER_FLAGS_COVERAGE
            ""
            CACHE STRING "Flags used for linking binaries during coverage builds."
            FORCE)
    SET(CMAKE_SHARED_LINKER_FLAGS_COVERAGE
            ""
            CACHE STRING "Flags used by the shared libraries linker during coverage builds."
            FORCE)
    MARK_AS_ADVANCED(
            CMAKE_CXX_FLAGS_COVERAGE
            CMAKE_C_FLAGS_COVERAGE
            CMAKE_EXE_LINKER_FLAGS_COVERAGE
            CMAKE_SHARED_LINKER_FLAGS_COVERAGE)


    # If unwanted files are included in the coverage reports, you can
    # adjust the exclude patterns on line 83.
    SETUP_TARGET_FOR_COVERAGE(
            coverage            # Name for custom target.
            ${TEST_NAME}        # Name of the test driver executable that runs the tests.
            # NOTE! This should always have a ZERO as exit code
            # otherwise the coverage generation will not complete.
            coverage_out        # Name of output directory.
    )
else ()
    add_custom_target(coverage
            COMMAND echo "${Red}Code coverage only available in coverage builds."
            COMMAND echo "${Green}Make a new build directory and rerun cmake with -DCMAKE_BUILD_TYPE=Coverage to enable this target.${ColorReset}"
            )
endif ()
