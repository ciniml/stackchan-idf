idf_component_register(
    SRCS "espnow_remote.cpp"
    INCLUDE_DIRS "include"
    REQUIRES "espressif__esp-now" "tl_expected"
    PRIV_REQUIRES "esp_wifi" "esp_netif" "esp_event" "nvs_flash")

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
