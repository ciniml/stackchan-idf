idf_component_register(
    SRCS
        "avatar.cpp"
        "balloon.cpp"
        "animation.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        M5GFX
        M5Unified
        avatar_vm
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
