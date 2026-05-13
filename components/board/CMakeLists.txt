idf_component_register(
    SRCS
        "board.cpp"
        "io_expander_py32.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        M5Unified
        M5GFX
    PRIV_REQUIRES
        esp_driver_i2c
        esp_driver_gpio
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)
