cmake_minimum_required(VERSION 3.22)

# Stack-chan Recovery — ADR-001 の「固定領域に置く小規模 Recovery アプリ」。
# BLE / Wi-Fi 経由で Main イメージを受信して書き込むことだけを行う。
# 表示・音声・サーボは持ち込まない (ADR-001「決定」参照)。
#
# 共有コンポーネントは ../components から必要最小限だけ取り込む。
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/tl_expected"
    "${CMAKE_CURRENT_LIST_DIR}/../components/flash_layout")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stackchan_recovery)
