cmake_minimum_required(VERSION 3.16)

# Upstream RT-Thread checkout. components/rtthread compiles src/ and include/
# from here unmodified; only components/rtthread/port/esp32s3/ belongs to this
# repository.
set(RTTHREAD_ROOT "I:/RT_thread/rt-thread" CACHE PATH "RT-Thread checkout")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s3_rtt_test)
