#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# Collect the bootloader / partition-table / ota_data / app binaries from a
# build directory and POST them to the remote-flasher server as one multipart
# /flash request. Streams the SSE response to stdout and exits non-zero on
# failure (including the "done" event carrying success:false).
#
# Section list and per-section offsets are read from `flasher_args.json` so
# this script tracks whatever idf.py decides to emit — no hardcoded paths.

set -euo pipefail

usage() {
    cat <<'EOF' >&2
Usage: flash-current-build.sh [options]

Options:
  -d, --build-dir DIR   Build directory (default: build-cores3)
  -u, --url URL         Server base URL (default: http://localhost:8765)
  -b, --baud N          Serial baud reported to esptool-js (default: 460800)
  -e, --erase           Set erase=true in meta (full chip erase first)
  -h, --help            Show this help

Environment:
  REMOTE_FLASHER_URL    Same as --url
  REMOTE_FLASHER_BAUD   Same as --baud

Exit codes:
  0   flash succeeded
  1   usage / local error
  2   HTTP error from the server (non-2xx, or stream truncated)
  3   server reported done with success:false (browser/esptool failure)
EOF
}

BUILD_DIR="build-cores3"
BASE_URL="${REMOTE_FLASHER_URL:-http://localhost:8765}"
BAUD="${REMOTE_FLASHER_BAUD:-460800}"
ERASE=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--build-dir) BUILD_DIR="$2"; shift 2 ;;
        -u|--url)       BASE_URL="$2"; shift 2 ;;
        -b|--baud)      BAUD="$2"; shift 2 ;;
        -e|--erase)     ERASE=true; shift ;;
        -h|--help)      usage; exit 0 ;;
        *)              echo "unknown arg: $1" >&2; usage; exit 1 ;;
    esac
done

command -v jq >/dev/null || { echo "jq is required" >&2; exit 1; }
command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }

ARGS_JSON="${BUILD_DIR}/flasher_args.json"
[[ -f "$ARGS_JSON" ]] || { echo "not found: $ARGS_JSON (did you run 'make build' / 'idf.py build'?)" >&2; exit 1; }

CHIP=$(jq -r '.extra_esptool_args.chip // "esp32s3"' "$ARGS_JSON")

# Build the section list from flasher_args.json. Each top-level key that has
# both .offset and .file is treated as a section (bootloader / app /
# partition-table / otadata). The field name in the multipart form matches the
# JSON key — the server uses that name to look up the corresponding part.
mapfile -t SECTION_KEYS < <(
    jq -r 'to_entries
        | map(select(.value | type == "object" and has("offset") and has("file")))
        | .[].key' "$ARGS_JSON"
)

if [[ ${#SECTION_KEYS[@]} -eq 0 ]]; then
    echo "no flashable sections found in $ARGS_JSON" >&2
    exit 1
fi

# Compose the meta JSON in declared order.
SECTIONS_JSON=$(
    for key in "${SECTION_KEYS[@]}"; do
        offset=$(jq -r --arg k "$key" '.[$k].offset' "$ARGS_JSON")
        printf '{"name":"%s","offset":"%s"}\n' "$key" "$offset"
    done | jq -s '.'
)
META=$(jq -nc \
    --arg chip "$CHIP" \
    --argjson baud "$BAUD" \
    --argjson erase "$ERASE" \
    --argjson sections "$SECTIONS_JSON" \
    '{chip:$chip, baud:$baud, erase:$erase, sections:$sections}')

# Build the curl -F arguments. `meta` field carries the JSON with the
# application/json content type so the server's parseMeta gets a string body.
CURL_FORM=(-F "meta=${META};type=application/json")
echo "== flashing ${CHIP} from ${BUILD_DIR} via ${BASE_URL} ==" >&2
for key in "${SECTION_KEYS[@]}"; do
    file=$(jq -r --arg k "$key" '.[$k].file' "$ARGS_JSON")
    path="${BUILD_DIR}/${file}"
    [[ -f "$path" ]] || { echo "missing binary: $path" >&2; exit 1; }
    size=$(stat -c %s "$path")
    offset=$(jq -r --arg k "$key" '.[$k].offset' "$ARGS_JSON")
    printf '  %-16s %8s  %7d B  %s\n' "$key" "$offset" "$size" "$file" >&2
    CURL_FORM+=(-F "${key}=@${path}")
done

# Fire the request. -N disables buffering so SSE events flow through
# immediately. The body is written to a tempfile (tee mirrors it to our
# stderr so the user sees progress live); the HTTP status is written via -w
# to a separate fd so it doesn't get mixed into the body capture.
RESPONSE_TMP=$(mktemp)
STATUS_TMP=$(mktemp)
trap 'rm -f "$RESPONSE_TMP" "$STATUS_TMP"' EXIT

curl -sS -N \
    -o >(tee "$RESPONSE_TMP" >&2) \
    -w '%{http_code}' \
    "${CURL_FORM[@]}" \
    "${BASE_URL}/flash" \
    > "$STATUS_TMP" || true
# tee races curl's exit; small sync so the tempfile is fully written.
sync
HTTP_CODE=$(<"$STATUS_TMP")

echo >&2
if [[ "$HTTP_CODE" != "200" ]]; then
    echo "server returned HTTP $HTTP_CODE" >&2
    exit 2
fi

# Walk the SSE stream we captured. The final event with type:"done" carries
# the authoritative success flag.
SUCCESS=
ERROR_MSG=
while IFS= read -r line; do
    [[ "$line" == data:* ]] || continue
    payload="${line#data: }"
    # Each event is a single JSON object on its own line.
    typ=$(printf '%s' "$payload" | jq -r '.type // empty' 2>/dev/null || true)
    if [[ "$typ" == "done" ]]; then
        SUCCESS=$(printf '%s' "$payload" | jq -r '.success')
        ERROR_MSG=$(printf '%s' "$payload" | jq -r '.error // ""')
    fi
done < "$RESPONSE_TMP"

if [[ -z "$SUCCESS" ]]; then
    echo "stream ended without a 'done' event" >&2
    exit 2
fi
if [[ "$SUCCESS" != "true" ]]; then
    echo "flash failed: ${ERROR_MSG:-(no error message)}" >&2
    exit 3
fi
echo "flash OK" >&2
exit 0
