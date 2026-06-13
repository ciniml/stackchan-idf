#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
#
# Subscribe to /monitor on the remote-flasher server and write each ESP
# serial line to stdout. The browser side must be running with Monitor
# active; without it the stream stays silent (you'll only see the initial
# `: monitor stream open` SSE comment).
#
# Ctrl+C unsubscribes cleanly.

set -euo pipefail

URL="${REMOTE_FLASHER_URL:-http://localhost:8765}"
SHOW_TS=false

usage() {
    cat <<'EOF' >&2
Usage: monitor-stream.sh [options]

Options:
  -u, --url URL    Server base URL (default: http://localhost:8765,
                   override via REMOTE_FLASHER_URL)
  -t, --timestamp  Prefix each line with the host's wall-clock time
  -h, --help       Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -u|--url)       URL="$2"; shift 2 ;;
        -t|--timestamp) SHOW_TS=true; shift ;;
        -h|--help)      usage; exit 0 ;;
        *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
    esac
done

command -v curl >/dev/null || { echo "curl is required" >&2; exit 1; }

# SSE frames look like `data: <line>\n\n`. Strip the `data: ` prefix; drop
# the comment line (`: ...`) the server sends as a flush probe and the
# blank separator lines. With -t, prepend a wall-clock timestamp.
if [[ "$SHOW_TS" == "true" ]]; then
    curl -sN "${URL}/monitor" \
        | awk '/^data: /{ sub(/^data: /, ""); cmd = "date +%H:%M:%S"; cmd | getline ts; close(cmd); print ts, $0; fflush() }'
else
    curl -sN "${URL}/monitor" \
        | sed -un 's/^data: //p'
fi
