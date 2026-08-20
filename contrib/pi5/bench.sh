#!/usr/bin/env bash
# Canonical Pi 5 xemu benchmark run. One invocation = one tagged run whose
# artifacts land in /home/pi/bench/<tag>.{csv,log,meta,done}; analyse and
# compare runs with scripts/perflog_report.py.
#
#   bench.sh -t TAG [-b gl|vk] [-a] [-m 1080p|native] [-s SECS] [-i ISO]
#
#   -t  tag: names the output files (required)
#   -b  presentation backend: gl (default path) or vk (XEMU_DISPLAY_BACKEND=vulkan)
#   -a  defer the flip-stall fence wait (XEMU_ASYNC_FLIP=1)
#   -m  output mode: 1080p (default; xrandr to 1920x1080) or native
#   -s  bounded run length in seconds (default 175)
#   -i  disc image (default: the Halo 2 benchmark ISO)
#   -r  reference PNG: capture a frame mid-run and check it against this
#       with scripts/frame_check.py, which sees flips and corruption that
#       the perflog cannot
#   -B  binary to run (default: the freshly built one). Set this to a saved
#       copy to A/B a change that is not switchable at runtime.
#
# Ground rules encoded here so a run cannot violate them:
#   - refuses to start if xemu is already running (someone may be playing)
#   - refuses if EmulationStation is running (it owns the display)
#   - the emulator is always bounded by timeout -s KILL
#   - X server locks are cleaned up on exit
set -uo pipefail

TAG="" BACKEND=gl ASYNC=0 MODE=1080p SECS=175 REFERENCE=""
ISO="/home/pi/RetroPie/roms/xbox/Halo 2 (XBCLASSICRP).iso"
BIN=/home/pi/xemu-build/dist/xemu
CFG=/home/pi/.local/share/xemu/xemu/xemu.toml
OUT=/home/pi/bench

while getopts "t:b:am:s:i:r:B:" o; do
    case $o in
        t) TAG=$OPTARG ;;
        b) BACKEND=$OPTARG ;;
        a) ASYNC=1 ;;
        m) MODE=$OPTARG ;;
        s) SECS=$OPTARG ;;
        i) ISO=$OPTARG ;;
        r) REFERENCE=$OPTARG ;;
        B) BIN=$OPTARG ;;
        *) exit 2 ;;
    esac
done
[ -n "$TAG" ] || { sed -n '2,20p' "$0"; exit 2; }
[ -x "$BIN" ] || { echo "no binary at $BIN"; exit 1; }
[ -r "$ISO" ] || { echo "no ISO at $ISO"; exit 1; }

if pgrep -x xemu >/dev/null; then
    echo "refusing: xemu is already running (someone may be playing)"; exit 1
fi
if pgrep -f "[e]mulationstation" >/dev/null; then
    echo "refusing: EmulationStation is running; stop it first"; exit 1
fi

mkdir -p "$OUT"
cleanup() {
    sudo pkill -9 -x matchbox-window-manager 2>/dev/null
    sudo pkill -9 -x X 2>/dev/null
    sudo rm -f /tmp/.X*-lock /tmp/.X11-unix/X* 2>/dev/null
    echo DONE > "$OUT/$TAG.done"
}
trap cleanup EXIT

export DISPLAY=:1
sudo X :1 -nolisten tcp vt8 >/dev/null 2>&1 &
sleep 8
if [ "$MODE" = 1080p ]; then
    xrandr --output "$(xrandr | awk '/ connected/{print $1; exit}')" \
        --mode 1920x1080 >/dev/null 2>&1
fi
matchbox-window-manager -use_cursor no >/dev/null 2>&1 &
sleep 4

ENV=(XEMU_PERFLOG="$OUT/$TAG.csv" XEMU_FPS=1)
[ "$BACKEND" = vk ] && ENV+=(XEMU_DISPLAY_BACKEND=vulkan)
[ "$ASYNC" = 1 ] && ENV+=(XEMU_ASYNC_FLIP=1)

{
    echo "date=$(date -Is)"
    echo "tag=$TAG backend=$BACKEND async=$ASYNC mode=$MODE secs=$SECS"
    echo "iso=$ISO"
    echo "binary=$(stat -c %y "$BIN")"
} > "$OUT/$TAG.meta"

env "${ENV[@]}" timeout -s KILL "$SECS" "$BIN" \
    -config_path "$CFG" -dvd_path "$ISO" > "$OUT/$TAG.log" 2>&1 &
XEMU_PID=$!

# Capture a frame once the guest is past boot, so a mirrored or corrupted
# picture is caught even though every timing number looks healthy.
if [ -n "$REFERENCE" ]; then
    sleep $((SECS / 3))
    import -window root "$OUT/$TAG.png" 2>/dev/null
fi
wait "$XEMU_PID"

if [ -n "$REFERENCE" ] && [ -f "$OUT/$TAG.png" ]; then
    echo "frame check against $(basename "$REFERENCE"):"
    python3 "$(dirname "$0")/../../scripts/frame_check.py" \
        "$OUT/$TAG.png" "$REFERENCE" || true
fi
echo "run complete: $OUT/$TAG.csv"
