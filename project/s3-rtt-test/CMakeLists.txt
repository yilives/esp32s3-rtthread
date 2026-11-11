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
        # Probe a few layouts instead of computing one path.
        #
        # This used to compute `<CMakeLists dir>/../../../rt-thread` and stop
        # there. In this repository's layout (project/s3-rtt-test/) that lands
        # on `<repo>/rt-thread`, which is not where a sibling checkout lives -
        # so a clean configure failed with "no RT-Thread checkout was found",
        # while a build directory that already had RTTHREAD_ROOT cached kept
        # working. That is the worst kind: it breaks only for new users.
        #
        # Candidates, in order of specificity:
        #   make:    <repo>/project/s3-rtt-test/ -> <repo>/../rt-thread
        #   in-repo: <repo>/project/s3-rtt-test/ -> <repo>/rt-thread
        #   flat:    <dir>/                      -> <dir>/../rt-thread
        set(_s3_rtt_candidates
            "${CMAKE_CURRENT_LIST_DIR}/../../../rt-thread"
            "${CMAKE_CURRENT_LIST_DIR}/../../rt-thread"
            "${CMAKE_CURRENT_LIST_DIR}/../rt-thread")
        set(RTTHREAD_ROOT "")
        foreach(_cand IN LISTS _s3_rtt_candidates)
            if(EXISTS "${_cand}/src/scheduler_mp.c")
                get_filename_component(RTTHREAD_ROOT "${_cand}" ABSOLUTE)
                break()
            endif()
        endforeach()
        if(RTTHREAD_ROOT STREQUAL "")
            message(FATAL_ERROR
                "RTTHREAD_ROOT is not set and no RT-Thread checkout was found. "
                "Looked for src/scheduler_mp.c under:\n"
                "  ${_s3_rtt_candidates}\n"
                "Pass -DRTTHREAD_ROOT=<path>, or set the RTTHREAD_ROOT environment "
                "variable, or place the checkout next to this repository.")
        endif()
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

# Publish the resolved path as a CACHE variable.
#
# components/rtthread/CMakeLists.txt reads it with
#
#   if(DEFINED CACHE{RTTHREAD_ROOT}) ... else() $ENV{RTTHREAD_ROOT}
#
# and a plain set() here is invisible to it - components are configured in a
# different scope. Without this a clean configure stops with
#
#   CMake Error at components/rtthread/CMakeLists.txt:48:
#     RTTHREAD_ROOT is not set.
#
# even though the message on the line above just printed the path. It only
# appeared to work while an old build directory still had the value cached.
set(RTTHREAD_ROOT "${RTTHREAD_ROOT}" CACHE PATH "RT-Thread checkout" FORCE)

message(STATUS "s3-rtt-test: RTTHREAD_ROOT=${RTTHREAD_ROOT}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s3_rtt_test)
