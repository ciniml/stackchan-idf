set(_main_priv_reqs
    board
    scs_servo
    avatar
    conversation
    config_service
    wifi_config_service
    jtts
    telegram
    espressif__esp_audio_codec
    json
    M5Unified
    M5GFX
    esp_timer
    esp_psram
    driver
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    app_update
    lwip
)

# Slim-build per-source gates. The PRIV_REQUIRES list above can't be made
# conditional on Kconfig (ESP-IDF's early-expansion phase evaluates REQUIRES
# before sdkconfig.h exists; the `if(CONFIG_*)` is dead at that point) — so
# we always link the conversation / audio_stream / wifi_audio components and
# just skip the per-file SRCS here, which trims out only the main-side glue
# (~10 KB) on the no-PSRAM build.
set(_main_srcs
    "ap_screen.cpp"
    "app_main.cpp"
    "atom_status.cpp"
    "audio_codec.cpp"
    "battery.cpp"
    "camera_service.cpp"
    "captive_portal.cpp"
    "demo_loop.cpp"
    "device_ui.cpp"
    "diag.cpp"
    "face_config.cpp"
    "i2c_dump.cpp"
    "led_task.cpp"
    "lt_timer.cpp"
    "mic_lip_sync_task.cpp"
    "qr_task.cpp"
    "render_task.cpp"
    "screens.cpp"
    "servo_limits.cpp"
    "servo_task.cpp"
    "settings_sinks.cpp"
    "speech.cpp"
    "wifi_sta.cpp"
)
if(CONFIG_STACKCHAN_CONVERSATION_ENABLED)
    list(APPEND _main_srcs "conversation_task.cpp")
endif()
if(CONFIG_STACKCHAN_AUDIO_STREAM_ENABLED)
    list(APPEND _main_srcs "audio_stream_sink.cpp")
endif()
if(CONFIG_STACKCHAN_WIFI_AUDIO_ENABLED)
    list(APPEND _main_srcs "wifi_audio.cpp")
endif()

# quirc (QR decoder) is only needed when the camera driver is compiled in.
# Same gating as components/board/CMakeLists.txt — keeps atoms3r builds free
# of the managed component download.
if(CONFIG_STACKCHAN_CAMERA_ENABLED)
    list(APPEND _main_priv_reqs espressif__quirc)
endif()

idf_component_register(
    SRCS ${_main_srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${_main_priv_reqs}
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
