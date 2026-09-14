# flash_layout — ADR-001 の拡張パーティションテーブル (exttab) と起動制御
# セクタ (bootctl) を Main / Recovery から扱う共通コンポーネント。
#
# format.c は ESP-IDF 非依存で、カスタムブートローダー
# (bootloader_components/main) とホストテスト (test/host) も同じファイルを
# コンパイルする。IDF 依存は flash_layout.cpp に閉じ込める。
idf_component_register(
    SRCS "format.c" "flash_layout.cpp"
    INCLUDE_DIRS "include"
    REQUIRES tl_expected esp_partition
    PRIV_REQUIRES log spi_flash
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
