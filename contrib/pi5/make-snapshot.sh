#!/usr/bin/env bash
# Capture a benchmark snapshot at a fixed point after boot.
#
#   make-snapshot.sh -n NAME -i ISO [-w SECONDS] [-B BIN]
#
# A/B runs are only comparable when both arms see the same guest work. Starting
# from a disc boot does not give that: the two arms drift apart during the intro
# and a scene difference then reads as a performance difference. Resuming a
# snapshot removes the drift, so this records one.
#
# The guest is driven through QMP rather than by hand, so the capture point is
# the same every time the snapshot is regenerated.
set -uo pipefail

NAME="" ISO="" WAIT=90 BIN=/home/pi/xemu-build/dist/xemu
CFG=/home/pi/.local/share/xemu/xemu/xemu.toml
SOCK=/tmp/xemu-qmp.sock

while getopts "n:i:w:B:" o; do
    case $o in
        n) NAME=$OPTARG ;;
        i) ISO=$OPTARG ;;
        w) WAIT=$OPTARG ;;
        B) BIN=$OPTARG ;;
        *) exit 2 ;;
    esac
done
[ -n "$NAME" ] && [ -n "$ISO" ] || { sed -n '2,12p' "$0"; exit 2; }
[ -r "$ISO" ] || { echo "no ISO at $ISO"; exit 1; }

pgrep -x xemu >/dev/null && { echo "xemu is already running; refusing"; exit 1; }
pgrep -f emulationstation >/dev/null && { echo "EmulationStation owns the display; stop it first"; exit 1; }

rm -f "$SOCK"
pkill -9 -x X 2>/dev/null; sleep 2
sudo X :1 -nolisten tcp vt8 >/tmp/x-snap.log 2>&1 &
sleep 8

XEMU_DISPLAY_BACKEND=vulkan DISPLAY=:1 timeout -s KILL $((WAIT + 120)) "$BIN" \
    -config_path "$CFG" -dvd_path "$ISO" \
    -qmp "unix:$SOCK,server,nowait" > /tmp/xemu-snap.log 2>&1 &
XEMU_PID=$!

echo "booting for ${WAIT}s before capturing '$NAME'"
sleep "$WAIT"

python3 - "$SOCK" "$NAME" <<'PY'
import json, socket, sys, time

sock_path, name = sys.argv[1], sys.argv[2]

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for attempt in range(20):
    try:
        s.connect(sock_path)
        break
    except OSError:
        time.sleep(1)
else:
    print("could not reach the monitor socket")
    sys.exit(1)

f = s.makefile("rw", encoding="utf-8", newline="\n")

def command(**payload):
    f.write(json.dumps(payload) + "\n")
    f.flush()
    while True:
        line = f.readline()
        if not line:
            return {}
        reply = json.loads(line)
        if "event" in reply:          # asynchronous, not our answer
            continue
        return reply

f.readline()                          # greeting
command(execute="qmp_capabilities")

# Pause first so the snapshot is a still point rather than a moving one.
command(execute="stop")
reply = command(execute="human-monitor-command",
                arguments={"command-line": f"savevm {name}"})
out = (reply.get("return") or "").strip()
print("  savevm:", out if out else "ok")
command(execute="quit")
PY

wait "$XEMU_PID" 2>/dev/null
pkill -9 -x xemu 2>/dev/null; sleep 2; sudo pkill -9 -x X 2>/dev/null
rm -f "$SOCK"

echo "snapshots now in the disk image:"
qemu-img snapshot -l /home/pi/.local/share/xemu/xemu/xbox_hdd.qcow2 2>&1 | sed 's/^/  /'
