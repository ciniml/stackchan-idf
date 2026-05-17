idf_component_register(
    SRCS
        "openai_realtime_client.cpp"
        "gemini_live_client.cpp"
        "base64.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        tl_expected
    PRIV_REQUIRES
        esp_websocket_client
        json
        mbedtls
        esp-tls
        esp_timer
        esp_event
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
