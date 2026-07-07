cmake_minimum_required(VERSION 3.16)
project(jtts_host_test C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(JTTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(EXPECTED_INC ${JTTS_ROOT}/../tl_expected/expected/include)
set(HTS_ROOT ${JTTS_ROOT}/../hts_engine)

# hts_engine (vendored, float 化済) — C ソース
file(GLOB HTS_SRCS ${HTS_ROOT}/lib/*.c)
add_library(hts_engine STATIC ${HTS_SRCS})
target_include_directories(hts_engine PUBLIC ${HTS_ROOT}/include PRIVATE ${HTS_ROOT}/lib)
target_compile_definitions(hts_engine PRIVATE AUDIO_PLAY_NONE)

add_library(jtts STATIC
    ${JTTS_ROOT}/src/kana_table.cpp
    ${JTTS_ROOT}/src/phoneme.cpp
    ${JTTS_ROOT}/src/formant_tables.cpp
    ${JTTS_ROOT}/src/formant_synth.cpp
    ${JTTS_ROOT}/src/formant_synth_classic.cpp
    ${JTTS_ROOT}/src/prosody.cpp
    ${JTTS_ROOT}/src/jtts.cpp
    ${JTTS_ROOT}/src/jvox.cpp
    ${JTTS_ROOT}/src/jvox_adpcm.cpp
    ${JTTS_ROOT}/src/unit_synth.cpp
    ${JTTS_ROOT}/src/hts_label.cpp
    ${JTTS_ROOT}/src/hmm_synth.cpp
)
target_include_directories(jtts
    PUBLIC ${JTTS_ROOT}/include ${EXPECTED_INC}
    PRIVATE ${JTTS_ROOT}/src
)
target_link_libraries(jtts PRIVATE hts_engine)
target_compile_options(jtts PRIVATE -Wall -Wextra)

add_executable(jtts_synth_demo main.cpp wav_writer.cpp)
target_link_libraries(jtts_synth_demo PRIVATE jtts)
target_compile_options(jtts_synth_demo PRIVATE -Wall -Wextra)

add_executable(jtts_gen_formant_t1 gen_formant_t1.cpp wav_writer.cpp)
target_link_libraries(jtts_gen_formant_t1 PRIVATE jtts)
target_compile_options(jtts_gen_formant_t1 PRIVATE -Wall -Wextra)

add_executable(jtts_test_vowels test_vowels.cpp wav_writer.cpp)
target_link_libraries(jtts_test_vowels PRIVATE jtts)
target_compile_options(jtts_test_vowels PRIVATE -Wall -Wextra)

# 単位連結 (PSOLA) エンジン: ブートストラップ単位の生成と検証。
add_executable(jtts_gen_units gen_units.cpp wav_writer.cpp)
target_link_libraries(jtts_gen_units PRIVATE jtts)
target_include_directories(jtts_gen_units PRIVATE ${JTTS_ROOT}/src)
target_compile_options(jtts_gen_units PRIVATE -Wall -Wextra)

add_executable(jtts_test_unit test_unit.cpp wav_writer.cpp)
target_link_libraries(jtts_test_unit PRIVATE jtts)
target_include_directories(jtts_test_unit PRIVATE ${JTTS_ROOT}/src)
target_compile_options(jtts_test_unit PRIVATE -Wall -Wextra)

add_executable(jtts_unit_demo unit_demo.cpp wav_writer.cpp)
target_link_libraries(jtts_unit_demo PRIVATE jtts)
target_include_directories(jtts_unit_demo PRIVATE ${JTTS_ROOT}/src)
target_compile_options(jtts_unit_demo PRIVATE -Wall -Wextra)

add_executable(jtts_test_adpcm test_adpcm.cpp wav_writer.cpp)
target_link_libraries(jtts_test_adpcm PRIVATE jtts)
target_compile_options(jtts_test_adpcm PRIVATE -Wall -Wextra)

# HMM エンジン: ラベル生成ダンプ + かな→WAV デモ
add_executable(jtts_hmm_demo hmm_demo.cpp wav_writer.cpp)
target_link_libraries(jtts_hmm_demo PRIVATE jtts)
target_include_directories(jtts_hmm_demo PRIVATE ${JTTS_ROOT}/src)
target_compile_options(jtts_hmm_demo PRIVATE -Wall -Wextra)
