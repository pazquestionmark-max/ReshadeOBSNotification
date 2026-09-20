#!/usr/bin/env bash
# Runs the real OBS script's server against the real overlay client.
#
# The two ends are written in different languages, by different code paths, so "they agree about
# the wire format" is a claim that has to be demonstrated rather than reviewed. This stands up
# the Python server exactly as OBS would, drives it with a scripted capture session, and checks
# that the C++ client -- the same one the add-on runs -- receives every event and turns it into
# the toast the overlay would draw.
#
# Usage: scripts/cross-language-check.sh [path-to-obsn-config]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROBE="${1:-$ROOT/build/tools/config-tool/obsn-config}"

if [[ ! -x "$PROBE" ]]; then
  echo "SKIP: $PROBE not built -- run cmake -S . -B build && cmake --build build" >&2
  exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: python3 not found" >&2
  exit 0
fi

ENDPOINT="${TMPDIR:-/tmp}/obsn-cross-$$.sock"
OUTPUT="$(mktemp)"
cleanup() {
  [[ -n "${PRODUCER:-}" ]] && kill "$PRODUCER" 2>/dev/null || true
  rm -f "$OUTPUT" "$ENDPOINT"
}
trap cleanup EXIT

python3 "$ROOT/scripts/cross_language_producer.py" "$ENDPOINT" 9 &
PRODUCER=$!

# The probe attaches, prints what arrives, and exits non-zero if it never heard anything.
"$PROBE" probe 9 "$ENDPOINT" | tee "$OUTPUT"
wait "$PRODUCER" || true
PRODUCER=""

status=0
require() {
  if ! grep -qF "$1" "$OUTPUT"; then
    echo "MISSING: $1" >&2
    status=1
  fi
}

# Every event kind the session produced must have arrived...
require "event: replay.started"
require "event: recording.started"
require "event: recording.paused"
require "event: recording.resumed"
require "event: replay.saved"
require "event: stream.started"
require "event: stream.reconnecting"
require "event: warning"
require "event: recording.stopped"
require "event: recording.saved"

# ...and each must have produced the toast the shipped configuration describes, with its
# placeholders filled from what the script actually sent.
require 'toast: "Replay buffer is on"'
require 'toast: "Recording started"'
require 'toast: "Recording paused"'
require 'toast: "Replay saved"  /  Replay 2026-09-20.mkv'
require 'toast: "Stream started"  /  Twitch'
require 'toast: "Reconnecting"  /  Attempt 1'
require 'toast: "Encoder overloaded: frames are being skipped"'
require 'toast: "Recording saved"  /  2026-09-20 21-14-03.mkv'

if [[ $status -eq 0 ]]; then
  echo
  echo "Cross-language check passed: the OBS script and the overlay agree."
fi
exit $status
