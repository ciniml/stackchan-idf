# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# Host-side (ESP-IDF independent) tests for the balloon scroll layout
# (components/avatar/balloon_layout.hpp — pure functions, header only).

cmake_minimum_required(VERSION 3.16)
project(avatar_host_test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(avatar_test_balloon_layout test_balloon_layout.cpp)
target_compile_options(avatar_test_balloon_layout PRIVATE -Wall -Wextra)
add_test(NAME balloon_layout COMMAND avatar_test_balloon_layout)
