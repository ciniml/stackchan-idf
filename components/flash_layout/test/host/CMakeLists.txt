# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# Host-side (ESP-IDF independent) unit tests for the ADR-001 exttab / bootctl
# format. Builds format.c straight against plain gcc — the same source the
# custom bootloader and the app compile.

cmake_minimum_required(VERSION 3.16)
project(flash_layout_host_test C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(FL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_library(fl_format STATIC ${FL_ROOT}/format.c)
target_include_directories(fl_format PUBLIC ${FL_ROOT}/include)
target_compile_options(fl_format PRIVATE -Wall -Wextra)

enable_testing()

add_executable(fl_test_format test_format.cpp)
target_link_libraries(fl_test_format PRIVATE fl_format)
target_compile_options(fl_test_format PRIVATE -Wall -Wextra)
add_test(NAME format COMMAND fl_test_format)
