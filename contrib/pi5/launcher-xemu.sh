#!/usr/bin/env bash
# Launch the locally built xemu for a RetroPie xbox entry.
# The stock package points at a flatpak; this uses the ARM64 build carrying
# the V3D fixes.
set -u
ROM="${1:-}"
BIN="/home/pi/xemu-build/dist/xemu"
CFG="/home/pi/.local/share/xemu/xemu/xemu.toml"
LOG="/home/pi/xemu-last.log"

[ -x "$BIN" ] || { echo "xemu binary not found at $BIN" >&2; exit 1; }
[ -n "$ROM" ] && [ -f "$ROM" ] || { echo "rom not found: $ROM" >&2; exit 1; }
[ -f "$CFG" ] || { echo "xemu config not found at $CFG" >&2; exit 1; }

# runcommand may invoke this through sudo, which leaves HOME as /root. xemu
# derives its BIOS, EEPROM and HDD paths from the config, so point at both
# explicitly rather than relying on the calling user.
export HOME=/home/pi
export XDG_DATA_HOME=/home/pi/.local/share

# Output at 1080p rather than the display native 4K. The Xbox renders at
# 640x480 either way so the visual difference is slight, but scaling to 4K
# loads the GPU enough to lengthen every surface download: measured 16.4 fps
# at 1080p against 13.2 at 4K on the same clocks, with the per-frame download
# falling from 18.65 to 15.25 ms.
if command -v xrandr >/dev/null 2>&1; then
    out=$(xrandr 2>/dev/null | awk "/ connected/{print \$1; exit}")
    if [ -n "$out" ]; then
        xrandr --output "$out" --mode 1920x1080 2>/dev/null || true
    fi
fi

# Record frame timings for every session, named so consecutive runs do not
# overwrite each other. The logger costs one branch per frame when disabled
# and its output is bounded.
if [ -z "${XEMU_PERFLOG:-}" ]; then
    name=$(basename "$ROM" | tr -c "A-Za-z0-9._-" "_" | cut -c1-40)
    export XEMU_PERFLOG="/home/pi/perflogs/${name}-$(date +%Y%m%d-%H%M%S).csv"
fi
export XEMU_FPS=1

# Keep the last run for diagnostics, but never let logging stop the game from
# starting: an unwritable redirect would abort the launch.
if ! : > "$LOG" 2>/dev/null; then
    LOG=/dev/null
fi

exec "$BIN" -config_path "$CFG" -dvd_path "$ROM" > "$LOG" 2>&1
