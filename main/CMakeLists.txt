set(_main_priv_reqs
    board
    crash_report
    scs_servo
    avatar
    conversation
    config_service
    wifi_config_service
    jtts
    telegram
    espressif__esp_audio_codec
    espressif__esp-sr
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
    espnow_poc
    espnow_remote
    flash_layout
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
    "asr_probe.cpp"
    "atom_status.cpp"
    "audio_codec.cpp"
    "battery.cpp"
    "camera_service.cpp"
    "dance.cpp"
    "dance_storage.cpp"
    "dance_poc.cpp"
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
    "voice_db.cpp"
    "hmm_voice.cpp"
    "sano_weights.cpp"
    "sano_bench.cpp"
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

# --- ADR-001: exttab / bootctl の生成と書き込み (partitions_main_*.csv のとき) --
# 開発時の `make flash` で Main と一緒に拡張テーブルと「起動先=Main (確認済み)」の
# bootctl を書く。Recovery は recovery/ プロジェクトから別途書く。
partition_table_get_partition_info(_exttab_off "--partition-name exttab" "offset")
if(_exttab_off)
    set(_gen "${PROJECT_DIR}/tools/flash_layout/gen_exttab.py")
    if(CONFIG_ESPTOOLPY_FLASHSIZE_16MB)
        set(_layout "${PROJECT_DIR}/exttab_16mb.json")
    else()
        set(_layout "${PROJECT_DIR}/exttab_8mb.json")
    endif()
    set(_exttab_bin "${CMAKE_BINARY_DIR}/exttab.bin")
    set(_bootctl_bin "${CMAKE_BINARY_DIR}/bootctl_main.bin")
    add_custom_command(
        OUTPUT "${_exttab_bin}"
        COMMAND ${python} "${_gen}" "${_layout}" --ab -o "${_exttab_bin}"
        DEPENDS "${_gen}" "${_layout}"
        COMMENT "[flash_layout] generating exttab from ${_layout}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${_bootctl_bin}"
        COMMAND ${python} "${_gen}" --bootctl main --ab -o "${_bootctl_bin}"
        DEPENDS "${_gen}"
        COMMENT "[flash_layout] generating bootctl (target=main, confirmed)"
        VERBATIM)
    add_custom_target(flash_layout_images ALL DEPENDS "${_exttab_bin}" "${_bootctl_bin}")
    partition_table_get_partition_info(_bootctl_off "--partition-name bootctl" "offset")
    esptool_py_flash_target_image(flash exttab "${_exttab_off}" "${_exttab_bin}")
    esptool_py_flash_target_image(flash bootctl "${_bootctl_off}" "${_bootctl_bin}")
endif()
