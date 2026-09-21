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
        "src/hts_label.cpp"
        "src/hmm_synth.cpp"
        "src/sano_ir.cpp"
        "src/clause_stream.cpp"
        "src/sano_visemes.cpp"
        "src/sano_synth.cpp"
        "src/hmm_chunk.cpp"
        "src/subtitle.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES tl_expected
    PRIV_REQUIRES hts_engine saanotts esp_hw_support
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
