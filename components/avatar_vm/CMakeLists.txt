idf_component_register(
    SRCS
        "decoder.cpp"
        "vm.cpp"
        "default_bytecode.cpp"
        "storage.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        M5GFX
        nvs_flash
        tl_expected
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

# --- default avatar face: compile the Lua-flavoured DSL to bytecode at build
# time and embed the result so the VM has a baseline face even with no NVS
# override. Both Node 18+ and the JS sources under tools/avatar_dsl/ must be
# available on the host running idf.py build (CI installs node before building).
find_program(NODE_EXECUTABLE NAMES node REQUIRED
    DOC "Node.js (>= 18) used to compile the avatar DSL at build time")

set(_AVATAR_DSL_DIR "${PROJECT_DIR}/tools/avatar_dsl")
set(_AVATAR_DSL_SRC "${PROJECT_DIR}/assets/default_face.avdsl")
set(_AVATAR_DSL_CLI "${_AVATAR_DSL_DIR}/cli.mjs")
set(_AVATAR_DSL_OUT "${CMAKE_CURRENT_BINARY_DIR}/default_face.avbc")

# Rebuild the bytecode whenever any compiler source or the DSL text changes.
file(GLOB _AVATAR_DSL_JS "${_AVATAR_DSL_DIR}/*.js" "${_AVATAR_DSL_DIR}/*.mjs")

add_custom_command(
    OUTPUT "${_AVATAR_DSL_OUT}"
    COMMAND ${NODE_EXECUTABLE} "${_AVATAR_DSL_CLI}" "${_AVATAR_DSL_SRC}" "${_AVATAR_DSL_OUT}"
    DEPENDS "${_AVATAR_DSL_SRC}" ${_AVATAR_DSL_JS}
    COMMENT "[avatar_vm] compiling default_face.avdsl -> default_face.avbc"
    VERBATIM
)
add_custom_target(avatar_vm_default_face DEPENDS "${_AVATAR_DSL_OUT}")
add_dependencies(${COMPONENT_LIB} avatar_vm_default_face)

# Linker symbols (per ESP-IDF EMBED docs):
#   _binary_default_face_avbc_start / _binary_default_face_avbc_end
target_add_binary_data(${COMPONENT_LIB} "${_AVATAR_DSL_OUT}" BINARY)
