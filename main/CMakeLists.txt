idf_component_register(
    SRCS
        "app_main.cpp"
        "render_task.cpp"
        "servo_task.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        board
        scs_servo
        avatar
        M5Unified
        M5GFX
        esp_timer
        esp_driver_uart
        esp_driver_gpio
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
