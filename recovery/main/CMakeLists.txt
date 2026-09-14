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
    PRIV_REQUIRES
        tl_expected
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
