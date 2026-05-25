#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# Build the avatar face renderer to a single self-contained HTML file using
# Emscripten (run from a Docker image — no host toolchain needed). The avatar
# C++ drawing + animation code is compiled verbatim against a tiny framebuffer
# M5GFX shim (wasm/shim/M5GFX.h); see wasm/avatar_wasm.cpp for the glue.
#
# Usage: wasm/build.sh            # build -> build/avatar_web/avatar.html
#        EMSDK_IMAGE=... wasm/build.sh   # override the emscripten image
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${EMSDK_IMAGE:-emscripten/emsdk:3.1.74}"
OUT_REL="build/avatar_web/avatar.html"

mkdir -p "$ROOT/build/avatar_web" "$ROOT/build/emcache"

EXPORTS='_avatar_init,_avatar_set_size,_avatar_width,_avatar_height,_avatar_framebuffer,_avatar_set_expression,_avatar_set_mouth,_avatar_set_manual_gaze,_avatar_set_saccade,_avatar_set_blink,_avatar_set_breath,_avatar_set_colors,_avatar_set_eyebrows_visible,_avatar_set_eye_params,_avatar_set_eyebrow_params,_avatar_set_mouth_params,_avatar_tick'

echo "[avatar-wasm] building with $IMAGE -> $OUT_REL"
docker run --rm \
  -v "$ROOT:/src" -w /src \
  --user "$(id -u):$(id -g)" \
  -e HOME=/tmp \
  -e EM_CACHE=/src/build/emcache \
  "$IMAGE" \
  emcc \
    -std=c++20 -O2 \
    -I wasm/shim -I components/avatar -I components/avatar/include \
    wasm/avatar_wasm.cpp \
    components/avatar/face.cpp \
    components/avatar/eye.cpp \
    components/avatar/mouth.cpp \
    components/avatar/eyebrow.cpp \
    components/avatar/effect.cpp \
    components/avatar/animation.cpp \
    -sSINGLE_FILE=1 \
    -sEXPORTED_FUNCTIONS="$EXPORTS" \
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU16 \
    -sALLOW_MEMORY_GROWTH=1 \
    -sENVIRONMENT=web \
    --shell-file wasm/shell.html \
    -o "$OUT_REL"

echo "[avatar-wasm] done: $ROOT/$OUT_REL"
