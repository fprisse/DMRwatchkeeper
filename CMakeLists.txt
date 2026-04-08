cmake_minimum_required(VERSION 3.16)
project(dmr_monitor VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined")

find_library(RTLSDR_LIB rtlsdr REQUIRED)
find_path(RTLSDR_INCLUDE rtl-sdr.h REQUIRED)
find_package(Threads REQUIRED)

message(STATUS "RTL-SDR library : ${RTLSDR_LIB}")
message(STATUS "RTL-SDR include : ${RTLSDR_INCLUDE}")

set(SOURCES
    src/main.cpp
    src/capture.cpp
    src/ringbuffer.cpp
    src/channel_filter.cpp
    src/fm_discriminator.cpp
    src/timing_recovery.cpp
    src/fsk_slicer.cpp
    src/demod_chain.cpp
)

add_executable(dmr_monitor ${SOURCES})

target_include_directories(dmr_monitor PRIVATE
    include
    ${RTLSDR_INCLUDE}
)

target_link_libraries(dmr_monitor PRIVATE
    ${RTLSDR_LIB}
    Threads::Threads
    m
)

target_compile_options(dmr_monitor PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
