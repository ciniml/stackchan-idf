idf_component_register(
    SRCS
        "src/kana_table.cpp"
        "src/phoneme.cpp"
        "src/formant_tables.cpp"
        "src/formant_synth.cpp"
        "src/formant_synth_classic.cpp"
        "src/prosody.cpp"
        "src/jtts.cpp"
        "src/jvox.cpp"
        "src/jvox_adpcm.cpp"
        "src/unit_synth.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES tl_expected
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
