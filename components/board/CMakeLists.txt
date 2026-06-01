idf_component_register(
    SRCS
        "board.cpp"
        "io_expander_py32.cpp"
        "led_strip.cpp"
        "si12t_touch.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        M5Unified
        M5GFX
        tl_expected
    PRIV_REQUIRES
        driver
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
