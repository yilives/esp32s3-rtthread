cmake_minimum_required(VERSION 3.16)

# Upstream RT-Thread checkout. components/rtthread compiles src/ and include/
# from here unmodified; only components/rtthread/port/ is part of this
# repository's own code.
#
# Resolution order, most specific first:
#   1. -DRTTHREAD_ROOT=<path> on the idf.py command line
#   2. the RTTHREAD_ROOT environment variable
#   3. a sibling checkout next to this repository (../rt-thread)
#
# Deliberately NOT hard-coded: a path baked into a committed file is only
# correct on the machine it was written on, and the failure it produces is
# confusing rather than loud - CMake keeps using the stale absolute path if it
# still exists, so a machine with several checkouts builds against the wrong one
# and shows changes that are not there. Measured while verifying this
# repository's own patch flow.
if(NOT DEFINED RTTHREAD_ROOT OR RTTHREAD_ROOT STREQUAL "")
    if(DEFINED ENV{RTTHREAD_ROOT} AND NOT "$ENV{RTTHREAD_ROOT}" STREQUAL "")
        set(RTTHREAD_ROOT "$ENV{RTTHREAD_ROOT}")
    else()
        get_filename_component(_s3_repo_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
        set(RTTHREAD_ROOT "${_s3_repo_root}/../rt-thread")
        if(NOT EXISTS "${RTTHREAD_ROOT}/src/scheduler_mp.c")
            message(FATAL_ERROR
                "RTTHREAD_ROOT is not set and no RT-Thread checkout was found at "
                "'${RTTHREAD_ROOT}'.\n"
                "Pass -DRTTHREAD_ROOT=<path>, or set the RTTHREAD_ROOT environment "
                "variable, or place the checkout next to this repository.")
        endif()
        get_filename_component(RTTHREAD_ROOT "${RTTHREAD_ROOT}" ABSOLUTE)
    endif()
endif()

if(NOT EXISTS "${RTTHREAD_ROOT}/src/scheduler_mp.c")
    message(FATAL_ERROR
        "RTTHREAD_ROOT='${RTTHREAD_ROOT}' does not look like an RT-Thread "
        "checkout (no src/scheduler_mp.c).")
endif()

# Normalise to forward slashes.
#
# RTTHREAD_ROOT ends up inside INCLUDE_DIRS, which ESP-IDF feeds to
# foreach()/list operations that treat backslash as an escape - a Windows path
# such as I:\RT_thread\_verify_rtt fails with
#
#   Invalid character escape '\R'.
#
# CMake accepts forward slashes on every platform, so this costs nothing and
# makes the variable safe to interpolate anywhere IDF might use it.
file(TO_CMAKE_PATH "${RTTHREAD_ROOT}" RTTHREAD_ROOT)
get_filename_component(RTTHREAD_ROOT "${RTTHREAD_ROOT}" ABSOLUTE)

message(STATUS "s3-rtt-test: RTTHREAD_ROOT=${RTTHREAD_ROOT}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s3_rtt_test)
