# crash_report — espcoredump (フラッシュ保存) の薄いラッパ。起動時の要約ログと
# HTTP API 用の JSON を提供する。coredump 領域は ADR-001 exttab の子として
# flash_layout::init() が登録するので、init() はその後に呼ぶ。
idf_component_register(
    SRCS "crash_report.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espcoredump
    PRIV_REQUIRES log esp_system esp_app_format
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
