# wifi_config_service — Wi-Fi-side configuration endpoint.
#
# Mirrors the BLE GATT settings service over HTTP once Wi-Fi STA is up.
# Same X25519 + AES-256-GCM application-layer crypto session as
# config_service/crypto.cpp, just framed in HTTP request/response bodies
# instead of GATT attribute reads/writes. mDNS publishes the hostname and
# `_stackchan-config._tcp.local` service for browser discovery.

idf_component_register(
    SRCS
        "wifi_config_service.cpp"
        "http_handlers.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES tl_expected config_service
    PRIV_REQUIRES
        esp_http_server
        esp_https_ota
        esp_app_format
        app_update
        esp_event
        esp_partition
        esp_system
        esp_timer
        json
        mbedtls
        espressif__mdns
    EMBED_TXTFILES "web/settings_wifi.html"
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
