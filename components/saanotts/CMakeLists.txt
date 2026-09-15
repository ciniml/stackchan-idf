# sanoTTS-jp 推論コア (C99、上流そのまま。upstream/UPSTREAM.md 参照)。
#
# int8 カーネルの構成は上流の esp32/components/saanotts_core と同じ既定:
#   ESP32-S3 → W8A8 + PIE (SAAN_INT8_ACT=1 SAAN_PIE=1)
#   それ以外 → W8A32 (移植可能 C99、fp32 とビット一致)
# W8A32 で測りたいときは -DSANOTTS_ENABLE_PIE=0。
set(_up "${CMAKE_CURRENT_LIST_DIR}/upstream")
idf_component_register(
    SRCS "${_up}/saanotts.c"
         "${_up}/saanotts_stream.c"
         "${_up}/fft.c"
         "${_up}/saanotts_int8.c"
         "${_up}/g2p.c"
    INCLUDE_DIRS "${_up}"
    PRIV_REQUIRES esp_common
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
# erf 表を内部 DRAM に置く配置属性 (上流の saan_port_esp32.h)。
target_compile_definitions(${COMPONENT_LIB} PRIVATE "SAAN_PORT_HEADER=\"saan_port_esp32.h\"")

if(NOT DEFINED SANOTTS_ENABLE_PIE)
    idf_build_get_property(_t IDF_TARGET)
    if(_t STREQUAL "esp32s3")
        set(SANOTTS_ENABLE_PIE 1)
    else()
        set(SANOTTS_ENABLE_PIE 0)
    endif()
endif()
if(SANOTTS_ENABLE_PIE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SAAN_INT8_ACT=1 SAAN_PIE=1)
    message(STATUS "sanoTTS: W8A8 + PIE")
else()
    message(STATUS "sanoTTS: W8A32 (scalar C99)")
endif()
