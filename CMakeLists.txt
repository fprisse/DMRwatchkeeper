cmake_minimum_required(VERSION 3.16)
project(dmr_monitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type default
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined")

# ── Dependencies ──────────────────────────────────────────────────────────────

# librtlsdr
find_library(RTLSDR_LIB rtlsdr REQUIRED)
find_path(RTLSDR_INCLUDE rtl-sdr.h REQUIRED)

# pthreads
find_package(Threads REQUIRED)

message(STATUS "RTL-SDR library : ${RTLSDR_LIB}")
message(STATUS "RTL-SDR include : ${RTLSDR_INCLUDE}")

# ── Sources ───────────────────────────────────────────────────────────────────

set(SOURCES
    src/main.cpp
    src/capture.cpp
    src/ringbuffer.cpp
)

add_executable(dmr_monitor ${SOURCES})

target_include_directories(dmr_monitor PRIVATE
    include
    ${RTLSDR_INCLUDE}
)

target_link_libraries(dmr_monitor PRIVATE
    ${RTLSDR_LIB}
    Threads::Threads
)

# Warnings
target_compile_options(dmr_monitor PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
