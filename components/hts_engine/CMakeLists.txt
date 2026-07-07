idf_component_register(
    SRCS
        "lib/HTS_audio.c"
        "lib/HTS_engine.c"
        "lib/HTS_gstream.c"
        "lib/HTS_label.c"
        "lib/HTS_misc.c"
        "lib/HTS_model.c"
        "lib/HTS_pstream.c"
        "lib/HTS_sstream.c"
        "lib/HTS_vocoder.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "lib"
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE AUDIO_PLAY_NONE)
# vendored C ソース: upstream 由来の警告は黙らせる (改変は最小限に留める)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=maybe-uninitialized -Wno-unused-but-set-variable)
