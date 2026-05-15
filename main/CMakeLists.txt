idf_component_register(
    SRCS
        "app_main.cpp"
        "audio_codec.cpp"
        "conversation_task.cpp"
        "render_task.cpp"
        "servo_task.cpp"
        "speech.cpp"
        "wifi_sta.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        board
        scs_servo
        avatar
        conversation
        config_service
        json
        M5Unified
        M5GFX
        esp_timer
        driver
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
