idf_component_register(
    SRCS "config_service.cpp" "gatt_settings.cpp" "config_store.cpp" "crypto.cpp" "dis.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES tl_expected
    PRIV_REQUIRES bt nvs_flash esp_system esp_event esp_timer mbedtls esp_app_format)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
