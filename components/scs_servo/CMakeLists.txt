idf_component_register(
    SRCS
        "scs_bus.cpp"
        "scs_servo.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_driver_uart
        esp_driver_gpio
        esp_timer
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)
