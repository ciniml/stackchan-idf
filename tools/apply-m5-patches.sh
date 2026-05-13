#!/usr/bin/env bash
# Apply local IDF 6.0 patches to the M5GFX and M5Unified submodules.
#
# Run after `git submodule update --init --recursive`. The submodules will
# show as dirty in `git status` afterwards (expected). To re-sync upstream,
# `git submodule update -f` resets them and you re-run this script.
#
# Once a fork is published these patches go away.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

apply_if_pristine() {
    local sub="$1"
    local patch="$2"
    local label="$3"

    if ! git -C "$root/components/$sub" diff --quiet; then
        echo "[$label] working tree already dirty — skipping (re-run after \`git submodule update -f\`)."
        return 0
    fi

    echo "[$label] applying $patch"
    git -C "$root/components/$sub" apply "$patch"
}

apply_if_pristine M5GFX     "$root/patches/m5gfx-idf6.patch"     M5GFX
apply_if_pristine M5Unified "$root/patches/m5unified-idf6.patch" M5Unified

echo "Done. Submodules now show as dirty in 'git status' — this is expected."
