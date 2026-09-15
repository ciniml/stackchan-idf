# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# sanoTTS-jp コアのホストテスト。上流の受け入れテスト (golden_test / stream_test) を
# ベンダリングしたソースに対してそのまま走らせ、取り込みとビルドフラグの正しさを
# 担保する。重み (非 MIT) はリポジトリに入れないので、SANOTTS_ASSETS に
#   saanotts-jp-v4-int8.bin / golden-v4-int8.bin
# を置いたディレクトリを渡したときだけテストを登録する (無ければビルドのみ)。
cmake_minimum_required(VERSION 3.16)
project(saanotts_host_test C)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(_up ${CMAKE_CURRENT_SOURCE_DIR}/../../upstream)
set(_core ${_up}/saanotts.c ${_up}/saanotts_stream.c ${_up}/fft.c ${_up}/saanotts_int8.c)

add_executable(golden_test ${_up}/golden_test.c ${_core})
target_include_directories(golden_test PRIVATE ${_up})
target_link_libraries(golden_test PRIVATE m)
add_executable(stream_test ${_up}/stream_test.c ${_core})
target_include_directories(stream_test PRIVATE ${_up})
target_link_libraries(stream_test PRIVATE m)
# W8A8 (活性 int8、PIE エミュレーション無し) の別バイナリ。
add_executable(stream_test_a8 ${_up}/stream_test.c ${_core})
target_include_directories(stream_test_a8 PRIVATE ${_up})
target_compile_definitions(stream_test_a8 PRIVATE SAAN_INT8_ACT=1)
target_link_libraries(stream_test_a8 PRIVATE m)

enable_testing()
set(SANOTTS_ASSETS "$ENV{SANOTTS_ASSETS}" CACHE PATH "dir with saanotts-jp-v4-int8.bin and golden-v4-int8.bin")
if(SANOTTS_ASSETS AND EXISTS "${SANOTTS_ASSETS}/saanotts-jp-v4-int8.bin")
    add_test(NAME golden_int8 COMMAND golden_test "${SANOTTS_ASSETS}/saanotts-jp-v4-int8.bin" "${SANOTTS_ASSETS}/golden-v4-int8.bin")
    add_test(NAME stream_w8a32 COMMAND stream_test "${SANOTTS_ASSETS}/saanotts-jp-v4-int8.bin" "${SANOTTS_ASSETS}/golden-v4-int8.bin")
    add_test(NAME stream_w8a8 COMMAND stream_test_a8 --g1-kb 176 "${SANOTTS_ASSETS}/saanotts-jp-v4-int8.bin" "${SANOTTS_ASSETS}/golden-v4-int8.bin")
else()
    message(STATUS "SANOTTS_ASSETS not set — building test binaries only")
endif()
