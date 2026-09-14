# ADR-001 カスタム 2 段目ブートローダー。IDF の bootloader/subproject/main を
# 丸ごと差し替える (bootloader_components/main は IDF が認識する上書き先)。
# recovery/ プロジェクトからも BOOTLOADER_EXTRA_COMPONENT_DIRS でこの
# ディレクトリを指すので、Main と Recovery のどちらを書き込んでも同じ
# ブートローダーになる。
#
# exttab / bootctl の形式と検証は components/flash_layout/format.c を
# そのまま流用する (IDF 非依存の C)。
set(_fl "${CMAKE_CURRENT_LIST_DIR}/../../components/flash_layout")

idf_component_register(
    SRCS "bootloader_start.c" "${_fl}/format.c"
    INCLUDE_DIRS "${_fl}/include"
    # bootloader_hooks.h は IDF の bootloader/subproject/main にしか無い。
    PRIV_INCLUDE_DIRS "$ENV{IDF_PATH}/components/bootloader/subproject/main"
    REQUIRES bootloader bootloader_support spi_flash
)

idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
