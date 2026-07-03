#!/usr/bin/env bash
# One-command ProFlame 2 evidence capture (Linux).
#
# Records everything needed to debug a session offline or attach to a bug
# report: decoded frames (JSON + human-readable), raw .cu8 samples of any
# undecoded bursts (replayable with `rtl_433 -r`), and environment info.
#
# Usage:
#   ./pf2_capture.sh [output-dir] [extra rtl_433 args...]
#   FREQ=315.07M ./pf2_capture.sh            # capture at a different frequency
#
# Press remote / ESP32 buttons while it runs; Ctrl-C to stop and summarize.
set -uo pipefail

FREQ="${FREQ:-315M}"
OUT="${1:-pf2-capture-$(date +%Y%m%d-%H%M%S)}"
shift 2>/dev/null || true

command -v rtl_433 >/dev/null || {
    echo "rtl_433 not found. Install it (apt install rtl-433 / brew install rtl_433)"
    echo "or use docker: docker run --device /dev/bus/usb/00X/00Y hertzg/rtl_433 ..."
    exit 1
}

mkdir -p "$OUT"
cd "$OUT"

{
    date -u +"%Y-%m-%dT%H:%M:%SZ"
    uname -a
    rtl_433 -V 2>&1 | head -n 1
    echo "freq=$FREQ extra_args=$*"
} > environment.txt

echo "Capturing to $(pwd)"
echo "  frames.json   - decoded ProFlame2 frames (rtl_433 JSON)"
echo "  frames.txt    - human-readable decodes"
echo "  g*.cu8        - raw samples of undecoded bursts (rtl_433 -r to replay)"
echo "Press remote/ESP buttons now. Ctrl-C to stop."
echo

# -S unknown: save raw samples only for bursts rtl_433 could NOT decode -
# exactly the ones you need for offline analysis when something is wrong.
rtl_433 -f "$FREQ" -R 207 -M time:iso -M level -S unknown \
        -F "json:frames.json" -F kv "$@" 2>rtl433.err | tee frames.txt

echo
echo "----- capture summary ($(pwd)) -----"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if command -v python3 >/dev/null && [ -s frames.json ]; then
    python3 "$SCRIPT_DIR/pf2_monitor.py" --file frames.json --no-color | tail -n 15
else
    echo "decoded frames: $(wc -l < frames.json 2>/dev/null || echo 0)"
fi
ls -1 ./*.cu8 2>/dev/null | head -n 5 || true
echo "Attach this whole directory to any bug report."
