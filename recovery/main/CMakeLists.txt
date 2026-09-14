# Recovery main — glue のみ。OTA 受信・書き込み、BLE セッション暗号、
# release-fetch の実体は Main と同じソースをそのまま流用する (サイズ計測が
# 実装コードと乖離しないようにするため)。将来は共通コンポーネントへ切り出す
# (ADR-001「共通のパーティション管理層」)。
set(_shared "${CMAKE_CURRENT_LIST_DIR}/../../components")

idf_component_register(
    SRCS
        "main.cpp"
        "wifi.cpp"
        "ble.cpp"
        "http.cpp"
        "${_shared}/config_service/ota.cpp"
        "${_shared}/config_service/crypto.cpp"
        "${_shared}/wifi_config_service/release_ota.cpp"
        "${_shared}/wifi_config_service/https_fetch.cpp"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS
        "${_shared}/config_service/include"
        "${_shared}/wifi_config_service"
        "${_shared}/wifi_config_service/include"
    PRIV_REQUIRES
        tl_expected
        flash_layout
        bt
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_http_server
        esp_http_client
        esp-tls
        mbedtls
        app_update
        esp_app_format
        esp_partition
        json
        esp_system
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)

# --- exttab / bootctl の生成と書き込み ----------------------------------------
# 拡張テーブル (A/B 同一内容 8 KiB) を exttab パーティション (0x190000) へ、
# 初期 bootctl (target=recovery、seq=1) を bootctl (0xd000) へ、idf.py flash で
# 一緒に書く。配置は ../exttab_<size>.json が正本。
set(_gen "${PROJECT_DIR}/../tools/flash_layout/gen_exttab.py")
if(CONFIG_ESPTOOLPY_FLASHSIZE_16MB)
    set(_layout "${PROJECT_DIR}/../exttab_16mb.json")
else()
    set(_layout "${PROJECT_DIR}/../exttab_8mb.json")
endif()
set(_exttab_bin "${CMAKE_BINARY_DIR}/exttab.bin")
set(_bootctl_bin "${CMAKE_BINARY_DIR}/bootctl_initial.bin")
add_custom_command(
    OUTPUT "${_exttab_bin}"
    COMMAND ${python} "${_gen}" "${_layout}" --ab -o "${_exttab_bin}"
    DEPENDS "${_gen}" "${_layout}"
    COMMENT "[flash_layout] generating exttab from ${_layout}"
    VERBATIM)
add_custom_command(
    OUTPUT "${_bootctl_bin}"
    COMMAND ${python} "${_gen}" --bootctl recovery --ab -o "${_bootctl_bin}"
    DEPENDS "${_gen}"
    COMMENT "[flash_layout] generating initial bootctl (target=recovery)"
    VERBATIM)
add_custom_target(flash_layout_images ALL DEPENDS "${_exttab_bin}" "${_bootctl_bin}")
partition_table_get_partition_info(_exttab_off "--partition-name exttab" "offset")
partition_table_get_partition_info(_bootctl_off "--partition-name bootctl" "offset")
esptool_py_flash_target_image(flash exttab "${_exttab_off}" "${_exttab_bin}")
esptool_py_flash_target_image(flash bootctl "${_bootctl_off}" "${_bootctl_bin}")
