set(_board_srcs
    "audio_module_es8388.cpp"
    "board.cpp"
    "camera_gc0308.cpp"
    "io_expander_py32.cpp"
    "led_strip.cpp"
    "nekomimi_led_strip.cpp"
    "si12t_touch.cpp"
)

# esp32-camera is pulled in only when STACKCHAN_CAMERA_ENABLED=y. Listing it
# unconditionally in PRIV_REQUIRES would force the managed component to be
# downloaded / linked even for the atoms3r profile (which has no GC0308). We
# expose it as an optional dependency the same way idf does for its own
# optional bits.
# espressif__led_strip is unconditional — both cores3 and atoms3r builds use
# RMT-driven WS2812 chains, and the managed component is small (~5 KiB).
set(_board_priv_reqs driver espressif__led_strip)
if(CONFIG_STACKCHAN_CAMERA_ENABLED)
    # Managed-component requires name keeps the upstream hyphen — IDF does NOT
    # rewrite "-" → "_" for component-manager dependencies; doing so makes the
    # esp_camera.h include silently miss.
    list(APPEND _board_priv_reqs espressif__esp32-camera)
endif()

idf_component_register(
    SRCS ${_board_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        M5Unified
        M5GFX
        tl_expected
    PRIV_REQUIRES ${_board_priv_reqs}
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
