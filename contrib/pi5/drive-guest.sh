#!/usr/bin/env bash
# Drive the guest with synthetic controller input and screenshot each step, so a
# benchmark can start from real gameplay instead of whatever the disc boots into.
#
#   drive-guest.sh -i ISO [-x SNAPSHOT] [-w BOOT_SECS] [-k "KEYS"] [-n SAVE_AS]
#
#   -k  semicolon-separated steps, each "key" or "key:repeat" or "wait:secs"
#       e.g. "wait:5;Return;wait:2;Down;Down;a;wait:10"
#   -n  after the sequence, save a snapshot under this name via QMP
#
# Xemu's default keyboard map: a/b/x/y are the face buttons, Return is Start,
# BackSpace is Back, arrows are the d-pad, e/s/f/d are the left stick. Those only
# reach the guest when a port is bound to "keyboard", which the owner's own
# config does not do, so this runs against a generated copy and never edits it.
set -uo pipefail

ISO="" SNAP="" BOOT=90 KEYS="" SAVE_AS=""
BIN=/home/pi/xemu-build/dist/xemu
SRC_CFG=/home/pi/.local/share/xemu/xemu/xemu.toml
CFG=/tmp/xemu-drive.toml
SHOTS=/tmp/drive
SOCK=/tmp/xemu-drive-qmp.sock

while getopts "i:x:w:k:n:B:" o; do
    case $o in
        i) ISO=$OPTARG ;;
        x) SNAP=$OPTARG ;;
        w) BOOT=$OPTARG ;;
        k) KEYS=$OPTARG ;;
        n) SAVE_AS=$OPTARG ;;
        B) BIN=$OPTARG ;;
        *) exit 2 ;;
    esac
done
[ -n "$ISO" ] || { sed -n '2,14p' "$0"; exit 2; }

pgrep -x xemu >/dev/null && { echo "xemu already running; refusing"; exit 1; }
pgrep -x emulationstation >/dev/null && { echo "stop EmulationStation first"; exit 1; }

# A copy with the keyboard bound to port 1. The owner's config is left alone.
python3 - "$SRC_CFG" "$CFG" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
text = open(src).read()
if re.search(r"^port1\s*=", text, re.M):
    text = re.sub(r"^port1\s*=.*$", "port1 = 'keyboard'", text, count=1, flags=re.M)
else:
    text = text.replace("[input.bindings]", "[input.bindings]\nport1 = 'keyboard'", 1)
open(dst, "w").write(text)
PY

rm -rf "$SHOTS"; mkdir -p "$SHOTS"
rm -f "$SOCK"
pkill -9 -x X 2>/dev/null; sleep 2
sudo X :1 -nolisten tcp vt8 >/tmp/x-drive.log 2>&1 &
sleep 8

XEMU_DISPLAY_BACKEND=vulkan DISPLAY=:1 timeout -s KILL $((BOOT + 400)) "$BIN" \
    -config_path "$CFG" -dvd_path "$ISO" ${SNAP:+-loadvm "$SNAP"} \
    -qmp "unix:$SOCK,server,nowait" > /tmp/xemu-drive.log 2>&1 &
XEMU_PID=$!

echo "settling for ${BOOT}s"
sleep "$BOOT"

shot () { DISPLAY=:1 import -window root "$SHOTS/$1.png" 2>/dev/null; echo "  shot $1"; }
shot 00_start

n=1
IFS=';' read -ra STEPS <<< "$KEYS"
for step in "${STEPS[@]}"; do
    [ -n "$step" ] || continue
    case "$step" in
        wait:*) sleep "${step#wait:}" ;;
        *:*)    key="${step%%:*}"; rep="${step##*:}"
                for _ in $(seq 1 "$rep"); do
                    DISPLAY=:1 xdotool key --clearmodifiers "$key"; sleep 0.4
                done ;;
        *)      DISPLAY=:1 xdotool key --clearmodifiers "$step"; sleep 0.8 ;;
    esac
    shot "$(printf '%02d' $n)_${step//:/_}"
    n=$((n + 1))
done

sleep 5
shot 99_final

if [ -n "$SAVE_AS" ]; then
    python3 - "$SOCK" "$SAVE_AS" <<'PY'
import json, socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(20):
    try:
        s.connect(sys.argv[1]); break
    except OSError:
        time.sleep(1)
else:
    print("  monitor socket unreachable"); sys.exit(1)
f = s.makefile("rw", encoding="utf-8", newline="\n")
def command(**p):
    f.write(json.dumps(p) + "\n"); f.flush()
    while True:
        line = f.readline()
        if not line:
            return {}
        r = json.loads(line)
        if "event" not in r:
            return r
f.readline()
command(execute="qmp_capabilities")
command(execute="stop")
r = command(execute="human-monitor-command",
            arguments={"command-line": f"savevm {sys.argv[2]}"})
print("  savevm:", (r.get("return") or "ok").strip() or "ok")
command(execute="quit")
PY
fi

pkill -9 -x xemu 2>/dev/null; sleep 2; sudo pkill -9 -x X 2>/dev/null
rm -f "$SOCK"
echo "screenshots in $SHOTS"
