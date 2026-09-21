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
target_compile_definitions(hts_engine PRIVATE AUDIO_PLAY_NONE HTS_EMBEDDED)

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
    ${JTTS_ROOT}/src/sano_ir.cpp
    ${JTTS_ROOT}/src/sano_synth.cpp
    ${JTTS_ROOT}/src/hmm_chunk.cpp
    ${JTTS_ROOT}/src/subtitle.cpp
)
target_include_directories(jtts
    PUBLIC ${JTTS_ROOT}/include ${EXPECTED_INC}
    PRIVATE ${JTTS_ROOT}/src
)
# sanoTTS-jp core (vendored, C99)
set(SAAN_ROOT ${JTTS_ROOT}/../saanotts/upstream)
add_library(saanotts STATIC
    ${SAAN_ROOT}/saanotts.c ${SAAN_ROOT}/saanotts_stream.c ${SAAN_ROOT}/fft.c
    ${SAAN_ROOT}/saanotts_int8.c ${SAAN_ROOT}/g2p.c)
target_include_directories(saanotts PUBLIC ${SAAN_ROOT})
target_link_libraries(saanotts PUBLIC m)

target_link_libraries(jtts PRIVATE hts_engine saanotts)
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

# sanoTTS エンジン: かな→中間表現の単体テスト + かな→WAV デモ (重みは引数で渡す)
add_executable(jtts_test_sano_ir test_sano_ir.cpp)
target_link_libraries(jtts_test_sano_ir PRIVATE jtts)
target_include_directories(jtts_test_sano_ir PRIVATE ${JTTS_ROOT}/src)
target_compile_options(jtts_test_sano_ir PRIVATE -Wall -Wextra)

add_executable(jtts_sano_demo sano_demo.cpp wav_writer.cpp)
target_link_libraries(jtts_sano_demo PRIVATE jtts)
target_compile_options(jtts_sano_demo PRIVATE -Wall -Wextra)

# フォルマント エンジンの口形イベント (リップシンク用)
add_executable(jtts_test_visemes test_visemes.cpp)
target_link_libraries(jtts_test_visemes PRIVATE jtts)
target_compile_options(jtts_test_visemes PRIVATE -Wall -Wextra)

# HMM 長文分割 (メモリ予算に応じたチャンク分割合成)
add_executable(jtts_test_hmm_chunk test_hmm_chunk.cpp)
target_link_libraries(jtts_test_hmm_chunk PRIVATE jtts)
target_include_directories(jtts_test_hmm_chunk PRIVATE ${JTTS_ROOT}/src)
target_compile_options(jtts_test_hmm_chunk PRIVATE -Wall -Wextra)
