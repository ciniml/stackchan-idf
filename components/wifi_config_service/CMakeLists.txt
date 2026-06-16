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
        "mcp_events.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES tl_expected config_service avatar_vm esp_http_server
    PRIV_REQUIRES
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
    # NB: settings_wifi.html is NOT listed in EMBED_TXTFILES; the in-page
    # Avatar DSL editor needs the JS compiler bundle inlined into it, which
    # is done by `tools/avatar_dsl/inject.mjs` at build time. The processed
    # file is then attached via target_add_binary_data below (same linker
    # symbol name — `_binary_settings_wifi_html_*` — because the basename
    # is unchanged).
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

# --- Build-time injection: bundle the DSL compiler + inline the default DSL
# source into a copy of settings_wifi.html under the build dir, then hand
# that copy to the binary embed step. IDF disallows add_custom_command during
# the requirements-collection phase, so this MUST sit after the
# idf_component_register() call.
find_program(NODE_EXECUTABLE NAMES node REQUIRED
    DOC "Node.js (>= 18) used to bundle the avatar DSL compiler into settings_wifi.html")

set(_SETTINGS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/settings_wifi.html")
set(_SETTINGS_OUT "${CMAKE_CURRENT_BINARY_DIR}/settings_wifi.html")
set(_DSL_INJECT "${PROJECT_DIR}/tools/avatar_dsl/inject.mjs")
# DSL presets shipped in the page selector. Order matters — the first preset
# is the textarea's initial value. Keep aligned with wasm/build.sh so the
# Wi-Fi page and the standalone preview offer the same lineup.
set(_DSL_PRESETS
    "default=${PROJECT_DIR}/assets/default_face.avdsl"
    "omega=${PROJECT_DIR}/assets/omega_mouth.avdsl"
    "aokko=${PROJECT_DIR}/assets/aokko_face.avdsl"
)
set(_DSL_PRESET_FILES
    "${PROJECT_DIR}/assets/default_face.avdsl"
    "${PROJECT_DIR}/assets/omega_mouth.avdsl"
    "${PROJECT_DIR}/assets/aokko_face.avdsl"
)
# The inject script depends on every JS source file it bundles, so rebuilds
# pick up edits to the compiler.
file(GLOB _DSL_JS "${PROJECT_DIR}/tools/avatar_dsl/*.js" "${PROJECT_DIR}/tools/avatar_dsl/*.mjs")

add_custom_command(
    OUTPUT "${_SETTINGS_OUT}"
    COMMAND ${NODE_EXECUTABLE} "${_DSL_INJECT}" "${_SETTINGS_SRC}" "${_SETTINGS_OUT}" ${_DSL_PRESETS}
    DEPENDS "${_SETTINGS_SRC}" ${_DSL_PRESET_FILES} ${_DSL_JS}
    COMMENT "[wifi_config_service] injecting avatar DSL compiler + presets into settings_wifi.html"
    VERBATIM
)
add_custom_target(wifi_settings_html_processed DEPENDS "${_SETTINGS_OUT}")
add_dependencies(${COMPONENT_LIB} wifi_settings_html_processed)
target_add_binary_data(${COMPONENT_LIB} "${_SETTINGS_OUT}" TEXT)
